#include "display.h"

#include <stdint.h>
#include <string.h>

#include "display_driver.h"
#include "display_internal.h"
#include "core/process/process.h"
#include "core_sdk/config.h"
#include "core_sdk/notification.h"
#include "core_sdk/process.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "bruce_display"
#define DISPLAY__MAX_CONTEXTS 16
#define DISPLAY__ROW_BUF_PIXELS                                                                              \
    (DISPLAY__NATIVE_WIDTH > DISPLAY__NATIVE_HEIGHT ? DISPLAY__NATIVE_WIDTH : DISPLAY__NATIVE_HEIGHT)

#if defined(CONFIG_BRUCE_BOARD_M5_CARDPUTER)
#define DISPLAY__DEFAULT_ROTATION 1
#elif defined(CONFIG_BRUCE_BOARD_M5_STICKC_PLUS2)
#define DISPLAY__DEFAULT_ROTATION 3
#else
#error "No Bruce board selected; set CONFIG_BRUCE_BOARD_* via menuconfig or sdkconfig"
#endif

typedef struct {
    bool active;
    char text[BRUCE_NOTIFICATION_TEXT_MAX];
    TickType_t expires_at;
    bruce_display_rect_t rect;
    uint32_t generation;
    uint32_t duration_ms;
} notification__state_t;

static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static bruce_display_color_t *s_framebuffer;
static DMA_ATTR bruce_display_color_t s_row_buffer[DISPLAY__ROW_BUF_PIXELS];
static bool s_dma_framebuffer;
static uint8_t s_rotation;
static int16_t s_fb_width;
static int16_t s_fb_height;
static uint8_t s_brightness;
static display__process_context_t s_contexts[DISPLAY__MAX_CONTEXTS];
static display__process_context_t s_system_context;
static display__process_context_t *s_draw_context;
static SemaphoreHandle_t s_transfer_done;
static bool s_dashboard_layout;
static notification__state_t s_notification;

static inline void display__lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static inline void display__unlock(void) { xSemaphoreGiveRecursive(s_mutex); }
static void display__ensure_lock(void);

bruce_result_t display__snapshot(
    uint16_t *pixels,
    size_t capacity,
    uint16_t *out_width,
    uint16_t *out_height,
    size_t *out_pixel_count
) {
    if (out_width == NULL || out_height == NULL || out_pixel_count == NULL ||
        (pixels == NULL && capacity != 0)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__ensure_lock();
    display__lock();
    if (!s_initialized || s_framebuffer == NULL) {
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    size_t count = (size_t)s_fb_width * (size_t)s_fb_height;
    *out_width = (uint16_t)s_fb_width;
    *out_height = (uint16_t)s_fb_height;
    *out_pixel_count = count;
    if (pixels != NULL && capacity < count) {
        display__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    if (pixels != NULL) memcpy(pixels, s_framebuffer, count * sizeof(*pixels));
    display__unlock();
    return BRUCE_OK;
}

static void display__ensure_lock(void) {
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateRecursiveMutex();
}

static bruce_display_rect_t display__fullscreen_rect(void) {
    return (bruce_display_rect_t){0, 0, s_fb_width, s_fb_height};
}

static void display__context_defaults(display__process_context_t *context) {
    context->text_color = BRUCE_COLOR_WHITE;
    context->text_bg_color = BRUCE_COLOR_BLACK;
    context->text_bg_transparent = false;
    context->text_size = 1;
    context->cursor_x = 0;
    context->cursor_y = 0;
}

static display__process_context_t *display__find_context_locked(bruce_process_id_t process_id) {
    if (process_id == BRUCE_PROCESS_ID_INVALID) return &s_system_context;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].process_id == process_id) return &s_contexts[i];
    }
    return NULL;
}

static bool display__rects_overlap(bruce_display_rect_t a, bruce_display_rect_t b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static bruce_display_rect_t display__rect_union(bruce_display_rect_t a, bruce_display_rect_t b) {
    if (a.width <= 0 || a.height <= 0) return b;
    if (b.width <= 0 || b.height <= 0) return a;
    int16_t left = a.x < b.x ? a.x : b.x;
    int16_t top = a.y < b.y ? a.y : b.y;
    int16_t right_a = a.x + a.width;
    int16_t right_b = b.x + b.width;
    int16_t bottom_a = a.y + a.height;
    int16_t bottom_b = b.y + b.height;
    int16_t right = right_a > right_b ? right_a : right_b;
    int16_t bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
    return (bruce_display_rect_t){left, top, right - left, bottom - top};
}

static bruce_display_rect_t display__notification_rect(const char *text) {
    int width = (int)strlen(text) * (DISPLAY__FONT_WIDTH + 1) + 8;
    if (width > s_fb_width - 4) width = s_fb_width - 4;
    if (width < 20) width = 20;
    return (bruce_display_rect_t){
        s_fb_width - width - 2, s_fb_height - DISPLAY__FONT_HEIGHT - 10, width, DISPLAY__FONT_HEIGHT + 8
    };
}

static bool display__overlay_conflicts_locked(bruce_display_rect_t rect) {
    if (s_system_context.frame_active && display__rects_overlap(s_system_context.viewport, rect)) return true;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active && !s_contexts[i].frame_noop &&
            display__rects_overlap(s_contexts[i].viewport, rect)) {
            return true;
        }
    }
    return false;
}

static void display__compose_notification_row(
    bruce_display_rect_t transfer, const notification__state_t *notification, int screen_y
) {
    bruce_display_rect_t rect = notification->rect;
    if (screen_y < rect.y || screen_y >= rect.y + rect.height) return;
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
        if (x < transfer.x || x >= transfer.x + transfer.width) continue;
        bool border = x == rect.x || screen_y == rect.y || x == rect.x + rect.width - 1 ||
                      screen_y == rect.y + rect.height - 1;
        s_row_buffer[x - transfer.x] = border ? BRUCE_COLOR_WHITE : BRUCE_COLOR_NAVY;
    }
    int text_base_y = rect.y + 4;
    if (screen_y < text_base_y || screen_y >= text_base_y + DISPLAY__FONT_HEIGHT) return;
    int font_row = screen_y - text_base_y;
    int cursor_x = rect.x + 4;
    for (const char *p = notification->text; *p != '\0'; ++p) {
        if (cursor_x + DISPLAY__FONT_WIDTH >= rect.x + rect.width - 3) break;
        const uint8_t *glyph = display_internal__font_glyph(*p);
        if (glyph != NULL) {
            for (int col = 0; col < DISPLAY__FONT_WIDTH; ++col) {
                int px = cursor_x + col;
                if ((glyph[col] & (1u << font_row)) && px >= transfer.x && px < transfer.x + transfer.width) {
                    s_row_buffer[px - transfer.x] = BRUCE_COLOR_WHITE;
                }
            }
        }
        cursor_x += DISPLAY__FONT_WIDTH + 1;
    }
}

static void display__set_visibility_locked(display__process_context_t *context) {
    bruce_display_rect_t next = {0};
    bool hidden = true;
    if (context->gui_requested && context->state == BRUCE_PROCESS_FOREGROUND) {
        next = display__fullscreen_rect();
        hidden = false;
    } else if (context->gui_requested && context->tiled && context->state == BRUCE_PROCESS_BACKGROUND) {
        next = context->viewport;
        hidden = false;
    }
    if (context->hidden != hidden || memcmp(&context->viewport, &next, sizeof(next)) != 0) {
        context->viewport = next;
        context->hidden = hidden;
        context->viewport_generation++;
    }
}

static display__process_context_t *display__drawing_context_locked(bruce_process_id_t caller) {
    display__process_context_t *context = display__find_context_locked(caller);
    if (context != NULL) s_draw_context = context;
    return context;
}

bruce_result_t display_internal__begin_draw(display__process_context_t **out_context) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__process_context_t *context = display__drawing_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    if (out_context != NULL) *out_context = context;
    return BRUCE_OK;
}

void display_internal__unlock(void) { display__unlock(); }

void display_internal__set_pixel(int16_t x, int16_t y, bruce_display_color_t color) {
    if (s_draw_context == NULL || s_draw_context->hidden || x < 0 || x >= s_draw_context->viewport.width ||
        y < 0 || y >= s_draw_context->viewport.height) {
        return;
    }
    int screen_x = s_draw_context->viewport.x + x;
    int screen_y = s_draw_context->viewport.y + y;
    if (screen_x >= 0 && screen_x < s_fb_width && screen_y >= 0 && screen_y < s_fb_height) {
        s_framebuffer[screen_y * s_fb_width + screen_x] = color;
    }
}

void display_internal__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (s_draw_context == NULL || s_draw_context->hidden) return;
    if (x + w > s_draw_context->viewport.width) w = s_draw_context->viewport.width - x;
    if (y + h > s_draw_context->viewport.height) h = s_draw_context->viewport.height - y;
    if (w <= 0 || h <= 0) return;
    x += s_draw_context->viewport.x;
    y += s_draw_context->viewport.y;
    for (int16_t row = y; row < y + h; ++row) {
        bruce_display_color_t *pixels = &s_framebuffer[row * s_fb_width + x];
        for (int16_t col = 0; col < w; ++col) pixels[col] = color;
    }
}

bool display_internal__on_transfer_done_from_isr(void) {
    BaseType_t high_priority_woken = pdFALSE;
    if (s_transfer_done != NULL) xSemaphoreGiveFromISR(s_transfer_done, &high_priority_woken);
    return high_priority_woken == pdTRUE;
}

/* Marks the notification as expired once its deadline passes.  Expiry is
 * lazy: the overlay pixels are repainted without the notification by the next
 * transfer that overlaps its rect, not by a dedicated timeout.  The caller
 * must hold the display lock. */
static void display__expire_notification_locked(void) {
    if (s_notification.active && (int32_t)(xTaskGetTickCount() - s_notification.expires_at) >= 0) {
        s_notification.active = false;
        s_notification.generation++;
    }
}

/* Pushes `rect` from the framebuffer to the panel, compositing the active
 * notification overlay into the transferred rows.  Runs synchronously in the
 * caller's task with the display lock held, so transfers are serialized and
 * can never race with drawing or with each other. */
static bruce_result_t display__transfer_locked(bruce_display_rect_t rect, bool fullscreen) {
#if CONFIG_BRUCE_QEMU_TEST_MODE
    (void)rect;
    (void)fullscreen;
    return BRUCE_OK;
#else
    display__expire_notification_locked();
    notification__state_t notification = s_notification;
    bool compose = notification.active && display__rects_overlap(rect, notification.rect);
    bool packed = !s_dma_framebuffer || !fullscreen || compose;
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
    if (packed) {
        for (int row = 0; row < rect.height; ++row) {
            int screen_y = rect.y + row;
            const bruce_display_color_t *pixels = &s_framebuffer[screen_y * s_fb_width + rect.x];
            if (compose || !s_dma_framebuffer) {
                memcpy(s_row_buffer, pixels, (size_t)rect.width * sizeof(*pixels));
                if (compose) display__compose_notification_row(rect, &notification, screen_y);
                pixels = s_row_buffer;
            }
            if (display_driver__draw_bitmap(rect.x, screen_y, rect.x + rect.width, screen_y + 1, pixels) !=
                    BRUCE_OK ||
                xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                return BRUCE_ERR_IO;
            }
        }
        return BRUCE_OK;
    }
    bruce_result_t result = display_driver__draw_bitmap(
        rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, s_framebuffer
    );
    if (result == BRUCE_OK && xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) result = BRUCE_ERR_IO;
    return result;
#endif
}

/* Caller must hold the display lock. */
static void display__finish_frame_locked(display__process_context_t *context) {
    context->frame_active = false;
    context->frame_noop = false;
    display__set_visibility_locked(context);
}

static void display__configure_rotation(void) {
    if (s_rotation == 0 || s_rotation == 2) {
        s_fb_width = DISPLAY__NATIVE_WIDTH;
        s_fb_height = DISPLAY__NATIVE_HEIGHT;
    } else {
        s_fb_width = DISPLAY__NATIVE_HEIGHT;
        s_fb_height = DISPLAY__NATIVE_WIDTH;
    }
    display_driver__configure_rotation(s_rotation);
}

static void display__release_resources_locked(void) {
    if (s_transfer_done != NULL) { vSemaphoreDelete(s_transfer_done); s_transfer_done = NULL; }
    if (s_framebuffer != NULL) { heap_caps_free(s_framebuffer); s_framebuffer = NULL; }
    display_driver__deinit();
}

bruce_result_t display__init(void) {
    display__ensure_lock();
    display__lock();
    if (s_initialized) { display__unlock(); return BRUCE_OK; }
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    bruce_result_t result = display_driver__init();
    if (result != BRUCE_OK) { display__unlock(); return result; }
#endif
    s_dma_framebuffer = config__get_display_dma_framebuffer();
    uint32_t framebuffer_caps = MALLOC_CAP_INTERNAL | (s_dma_framebuffer ? MALLOC_CAP_DMA : MALLOC_CAP_8BIT);
    s_framebuffer = heap_caps_malloc(DISPLAY__FB_SIZE, framebuffer_caps);
    if (s_framebuffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate framebuffer");
        display_driver__deinit();
        display__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    s_rotation = DISPLAY__DEFAULT_ROTATION;
    display__configure_rotation();
    memset(&s_system_context, 0, sizeof(s_system_context));
    memset(&s_notification, 0, sizeof(s_notification));
    s_system_context.in_use = true;
    s_system_context.gui_requested = true;
    s_system_context.state = BRUCE_PROCESS_FOREGROUND;
    s_system_context.viewport = display__fullscreen_rect();
    display__context_defaults(&s_system_context);
    s_transfer_done = xSemaphoreCreateBinary();
    if (s_transfer_done == NULL) {
        display__release_resources_locked();
        display__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use) display__set_visibility_locked(&s_contexts[i]);
    }
    memset(s_framebuffer, 0, DISPLAY__FB_SIZE);
    int configured_brightness = config__get_bright();
    s_brightness = (uint8_t)((configured_brightness * 255) / 100);
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    display_driver__set_backlight(s_brightness);
#endif
    s_initialized = true;
    display__unlock();
    display__flush();
    return BRUCE_OK;
}

void display__deinit(void) {
    if (s_mutex == NULL) return;
    display__lock();
    if (!s_initialized) { display__unlock(); return; }
    s_initialized = false;
    display__release_resources_locked();
    display__unlock();
}

int display__width(void) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    display__process_context_t *context = display__find_context_locked(caller);
    int width = context != NULL && !context->hidden ? context->viewport.width : 0;
    display__unlock();
    return width;
}

int display__height(void) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    display__process_context_t *context = display__find_context_locked(caller);
    int height = context != NULL && !context->hidden ? context->viewport.height : 0;
    display__unlock();
    return height;
}

bruce_result_t display__set_rotation(uint8_t rotation) {
    bruce_process_id_t caller_id = process__current_id();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    display__process_context_t *caller = display__find_context_locked(caller_id);
    if (caller == NULL || caller->tiled) { display__unlock(); return BRUCE_ERR_PERMISSION; }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active) { display__unlock(); return BRUCE_ERR_BUSY; }
    }
    if (s_system_context.frame_active) { display__unlock(); return BRUCE_ERR_BUSY; }
    s_rotation = rotation & 3;
    display__configure_rotation();
    if (s_notification.active) {
        s_notification.rect = display__notification_rect(s_notification.text);
        s_notification.generation++;
    }
    memset(s_framebuffer, 0, DISPLAY__FB_SIZE);
    s_system_context.viewport = display__fullscreen_rect();
    s_system_context.viewport_generation++;
    s_dashboard_layout = false;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) continue;
        s_contexts[i].tiled = false;
        display__set_visibility_locked(&s_contexts[i]);
    }
    display__unlock();
    return BRUCE_OK;
}

uint8_t display__get_rotation(void) {
    display__lock();
    uint8_t rotation = s_rotation;
    display__unlock();
    return rotation;
}

bruce_result_t display__invert_display(bool invert) {
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
#if CONFIG_BRUCE_QEMU_TEST_MODE
    bruce_result_t result = BRUCE_OK;
#else
    (void)display_driver__invert(invert);
    bruce_result_t result = BRUCE_OK;
#endif
    display__unlock();
    return result;
}

bruce_result_t display__set_brightness(uint8_t brightness) {
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    display__unlock();
    bruce_result_t result = config__set_bright((int)brightness * 100 / 255);
    if (result == BRUCE_OK) {
        display__lock();
        if (!s_initialized) {
            display__unlock();
            return BRUCE_ERR_NOT_INITIALIZED;
        }
        s_brightness = brightness;
#if !CONFIG_BRUCE_QEMU_TEST_MODE
        display_driver__set_backlight(brightness);
#endif
        display__unlock();
    }
    return result;
}

uint8_t display__get_brightness(void) {
    display__lock();
    uint8_t brightness = s_brightness;
    display__unlock();
    return brightness;
}

bruce_result_t display__display_on_off(bool on) {
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
#if CONFIG_BRUCE_QEMU_TEST_MODE
    bruce_result_t result = BRUCE_OK;
#else
    (void)display_driver__set_enabled(on);
    bruce_result_t result = BRUCE_OK;
#endif
    display__unlock();
    return result;
}

bruce_result_t display__begin_frame(void) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    display__process_context_t *context = display__find_context_locked(caller);
    if (context == NULL) { display__unlock(); return BRUCE_ERR_PERMISSION; }
    if (context->frame_active) { display__unlock(); return BRUCE_ERR_INVALID_STATE; }
    context->frame_active = true;
    context->frame_noop = context->hidden;
    context->frame_generation = context->viewport_generation;
    if (!context->frame_noop) {
        for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
            display__process_context_t *other = &s_contexts[i];
            if (other != context && other->in_use && other->frame_active && !other->frame_noop &&
                display__rects_overlap(context->viewport, other->viewport)) {
                context->frame_active = false;
                display__unlock();
                return BRUCE_ERR_BUSY;
            }
        }
        if (context->clear_on_next_frame) {
            s_draw_context = context;
            display_internal__fill_rect(0, 0, context->viewport.width, context->viewport.height, BRUCE_COLOR_BLACK);
            context->clear_on_next_frame = false;
        }
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__present(void) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    display__process_context_t *context = display__find_context_locked(caller);
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    if (context == NULL || !context->frame_active) { display__unlock(); return BRUCE_ERR_INVALID_STATE; }
    if (context->frame_noop) {
        display__finish_frame_locked(context);
        display__unlock();
        return BRUCE_OK;
    }
    if (context->frame_generation != context->viewport_generation) {
        display__finish_frame_locked(context);
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    bool fullscreen = context->viewport.x == 0 && context->viewport.y == 0 &&
                      context->viewport.width == s_fb_width && context->viewport.height == s_fb_height;
    bruce_result_t result = display__transfer_locked(context->viewport, fullscreen);
    display__finish_frame_locked(context);
    display__unlock();
    return result;
}

bruce_result_t display__flush(void) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    display__process_context_t *context = display__find_context_locked(caller);
    bool active = context != NULL && context->frame_active;
    display__unlock();
    if (!active) {
        bruce_result_t result = display__begin_frame();
        if (result != BRUCE_OK) return result;
    }
    return display__present();
}

bruce_result_t display__set_tiles(const bruce_display_tile_t *tiles, size_t count) {
    bool built_in = false;
    if ((count > 0 && tiles == NULL) || count > BRUCE_DISPLAY_MAX_TILES ||
        process_registry__current_context(&built_in, NULL, 0, NULL) != BRUCE_OK || !built_in) {
        return BRUCE_ERR_PERMISSION;
    }
    bruce_process_id_t caller = process__current_id();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    display__process_context_t *owner = display__find_context_locked(caller);
    if (owner == NULL || owner->state != BRUCE_PROCESS_FOREGROUND) {
        display__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    display__process_context_t *targets[BRUCE_DISPLAY_MAX_TILES] = {0};
    for (size_t i = 0; i < count; ++i) {
        bruce_display_rect_t rect = tiles[i].rect;
        if (rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0 ||
            rect.x + rect.width > s_fb_width || rect.y + rect.height > s_fb_height) {
            display__unlock();
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        targets[i] = display__find_context_locked(tiles[i].process_id);
        if (targets[i] == NULL || !targets[i]->gui_requested || targets[i]->state != BRUCE_PROCESS_BACKGROUND) {
            display__unlock();
            return BRUCE_ERR_NOT_FOUND;
        }
        for (size_t j = 0; j < i; ++j) {
            if (tiles[j].process_id == tiles[i].process_id || display__rects_overlap(tiles[j].rect, rect)) {
                display__unlock();
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
        }
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active && s_contexts[i].tiled) {
            display__unlock();
            return BRUCE_ERR_BUSY;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (targets[i]->frame_active) { display__unlock(); return BRUCE_ERR_BUSY; }
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) continue;
        s_contexts[i].tiled = false;
        display__set_visibility_locked(&s_contexts[i]);
    }
    for (size_t i = 0; i < count; ++i) {
        targets[i]->tiled = true;
        targets[i]->viewport = tiles[i].rect;
        targets[i]->hidden = false;
        targets[i]->viewport_generation++;
        for (int row = 0; row < tiles[i].rect.height; ++row) {
            memset(
                &s_framebuffer[(tiles[i].rect.y + row) * s_fb_width + tiles[i].rect.x],
                0,
                (size_t)tiles[i].rect.width * sizeof(*s_framebuffer)
            );
        }
    }
    s_dashboard_layout = count > 0;
    display__unlock();
    return BRUCE_OK;
}

void display__process_created(bruce_process_id_t process_id, bool gui_requested) {
    display__ensure_lock();
    display__lock();
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(s_contexts[i]));
            s_contexts[i].in_use = true;
            s_contexts[i].process_id = process_id;
            s_contexts[i].gui_requested = gui_requested;
            s_contexts[i].hidden = true;
            display__context_defaults(&s_contexts[i]);
            break;
        }
    }
    display__unlock();
}

void display__process_set_gui_requested(bruce_process_id_t process_id) {
    display__ensure_lock();
    display__lock();
    display__process_context_t *context = display__find_context_locked(process_id);
    if (context != NULL && !context->gui_requested) {
        context->gui_requested = true;
        if (context->state == BRUCE_PROCESS_FOREGROUND) context->clear_on_next_frame = true;
        if (!context->frame_active) display__set_visibility_locked(context);
    }
    display__unlock();
}

void display__process_state_changed(bruce_process_id_t process_id, bruce_process_state_t state) {
    display__ensure_lock();
    display__lock();
    display__process_context_t *context = display__find_context_locked(process_id);
    if (context != NULL) {
        if (context->gui_requested && state == BRUCE_PROCESS_FOREGROUND && context->state != BRUCE_PROCESS_FOREGROUND) {
            context->clear_on_next_frame = true;
        }
        context->state = state;
        if (state != BRUCE_PROCESS_FOREGROUND && context->frame_active && !context->tiled) context->frame_noop = true;
        if (!context->frame_active) display__set_visibility_locked(context);
    }
    display__unlock();
}

void display__process_removed(bruce_process_id_t process_id) {
    display__ensure_lock();
    display__lock();
    display__process_context_t *context = display__find_context_locked(process_id);
    if (context != NULL) {
        context->hidden = true;
        context->tiled = false;
        context->frame_active = false;
        context->in_use = false;
    }
    display__unlock();
}

bruce_result_t display__test_read_pixel(int16_t x, int16_t y, bruce_display_color_t *out_color) {
    if (out_color == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    display__lock();
    if (!s_initialized || x < 0 || y < 0 || x >= s_fb_width || y >= s_fb_height) {
        display__unlock();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_color = s_framebuffer[y * s_fb_width + x];
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__notification_push(const char *text, uint32_t duration_ms) {
    if (text == NULL || text[0] == '\0' || strlen(text) >= BRUCE_NOTIFICATION_TEXT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (duration_ms < BRUCE_NOTIFICATION_DURATION_MIN_MS) duration_ms = BRUCE_NOTIFICATION_DURATION_MIN_MS;
    if (duration_ms > BRUCE_NOTIFICATION_DURATION_MAX_MS) duration_ms = BRUCE_NOTIFICATION_DURATION_MAX_MS;
    display__ensure_lock();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    bruce_display_rect_t old_rect = s_notification.active ? s_notification.rect : (bruce_display_rect_t){0};
    strncpy(s_notification.text, text, sizeof(s_notification.text) - 1);
    s_notification.text[sizeof(s_notification.text) - 1] = '\0';
    s_notification.active = true;
    s_notification.duration_ms = duration_ms;
    s_notification.expires_at = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    s_notification.rect = display__notification_rect(s_notification.text);
    s_notification.generation++;
    bruce_result_t result = BRUCE_OK;
    bruce_display_rect_t repaint = display__rect_union(old_rect, s_notification.rect);
    if (!display__overlay_conflicts_locked(repaint)) result = display__transfer_locked(repaint, false);
    display__unlock();
    return result;
}

bruce_result_t display__notification_dismiss(void) {
    display__ensure_lock();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    if (!s_notification.active) { display__unlock(); return BRUCE_OK; }
    bruce_display_rect_t repaint = s_notification.rect;
    s_notification.active = false;
    s_notification.generation++;
    bruce_result_t result = BRUCE_OK;
    if (!display__overlay_conflicts_locked(repaint)) result = display__transfer_locked(repaint, false);
    display__unlock();
    return result;
}

bruce_result_t display__test_notification(
    char *text, size_t text_size, bool *active, uint32_t *duration_ms, bruce_display_rect_t *rect,
    uint32_t *generation
) {
    if (text == NULL || text_size == 0 || active == NULL || duration_ms == NULL || rect == NULL ||
        generation == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__lock();
    strncpy(text, s_notification.text, text_size - 1);
    text[text_size - 1] = '\0';
    *active = s_notification.active;
    *duration_ms = s_notification.duration_ms;
    *rect = s_notification.rect;
    *generation = s_notification.generation;
    display__unlock();
    return BRUCE_OK;
}
