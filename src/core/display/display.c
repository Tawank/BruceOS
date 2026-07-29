#include "display.h"

#include <stdint.h>
#include <string.h>

#include "display_driver.h"
#include "display_internal.h"
#include "core/task/task.h"
#include "core_sdk/config.h"
#include "core_sdk/notification.h"
#include "core_sdk/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "bruce_display"
#define DISPLAY__MAX_CONTEXTS 16
#define DISPLAY__WORKER_STACK 4096
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
    display__task_context_t *context;
    bruce_display_rect_t rect;
    bool fullscreen;
    bool overlay_update;
    bool shutdown;
    uint32_t notification_generation;
} display__request_t;

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
static bruce_display_color_t s_row_buffer[DISPLAY__ROW_BUF_PIXELS];
static uint8_t s_rotation;
static int16_t s_fb_width;
static int16_t s_fb_height;
static uint8_t s_brightness;
static display__task_context_t s_contexts[DISPLAY__MAX_CONTEXTS];
static display__task_context_t s_system_context;
static display__task_context_t *s_draw_context;
static QueueHandle_t s_request_queue;
static SemaphoreHandle_t s_transfer_done;
static SemaphoreHandle_t s_worker_stopped;
static TaskHandle_t s_worker_task;
static bool s_dashboard_layout;
static notification__state_t s_notification;
static bool s_transfer_active;

static inline void display__lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static inline void display__unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static void display__ensure_lock(void) {
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateRecursiveMutex();
}

static bruce_display_rect_t display__fullscreen_rect(void) {
    return (bruce_display_rect_t){0, 0, s_fb_width, s_fb_height};
}

static void display__context_defaults(display__task_context_t *context) {
    context->text_color = BRUCE_COLOR_WHITE;
    context->text_bg_color = BRUCE_COLOR_BLACK;
    context->text_bg_transparent = false;
    context->text_size = 1;
    context->cursor_x = 0;
    context->cursor_y = 0;
}

static display__task_context_t *display__find_context_locked(bruce_task_id_t task_id) {
    if (task_id == BRUCE_TASK_ID_INVALID) return &s_system_context;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].task_id == task_id) return &s_contexts[i];
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

static void display__set_visibility_locked(display__task_context_t *context) {
    bruce_display_rect_t next = {0};
    bool hidden = true;
    if (context->gui_requested && context->state == BRUCE_TASK_FOREGROUND) {
        next = display__fullscreen_rect();
        hidden = false;
    } else if (context->gui_requested && context->tiled && context->state == BRUCE_TASK_BACKGROUND) {
        next = context->viewport;
        hidden = false;
    }
    if (context->hidden != hidden || memcmp(&context->viewport, &next, sizeof(next)) != 0) {
        context->viewport = next;
        context->hidden = hidden;
        context->viewport_generation++;
    }
}

static display__task_context_t *display__drawing_context_locked(bruce_task_id_t caller) {
    display__task_context_t *context = display__find_context_locked(caller);
    if (context != NULL) s_draw_context = context;
    return context;
}

bruce_result_t display_internal__begin_draw(display__task_context_t **out_context) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__task_context_t *context = display__drawing_context_locked(caller);
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

static void display__finish_request(display__request_t *request, bruce_result_t result) {
    if (request->overlay_update) return;
    display__lock();
    display__task_context_t *context = request->context;
    context->completion_result = result;
    context->transfer_pending = false;
    context->frame_active = false;
    context->frame_noop = false;
    if (!context->remove_pending) display__set_visibility_locked(context);
    if (context->remove_pending && context != &s_system_context) {
        SemaphoreHandle_t completion = context->completion;
        context->completion = NULL;
        context->in_use = false;
        context->remove_pending = false;
        display__unlock();
        if (completion != NULL) vSemaphoreDelete(completion);
        return;
    }
    xSemaphoreGive(context->completion);
    display__unlock();
}

static void display__worker(void *arg) {
    (void)arg;
    display__request_t request;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        display__lock();
        if (s_notification.active) {
            TickType_t now = xTaskGetTickCount();
            wait = (int32_t)(s_notification.expires_at - now) > 0 ? s_notification.expires_at - now : 0;
        }
        display__unlock();
        if (xQueueReceive(s_request_queue, &request, wait) != pdPASS) {
            display__lock();
            if (!s_notification.active || (int32_t)(xTaskGetTickCount() - s_notification.expires_at) < 0) {
                display__unlock();
                continue;
            }
            request = (display__request_t){
                .rect = s_notification.rect,
                .overlay_update = true,
                .notification_generation = s_notification.generation,
            };
            s_notification.active = false;
            s_notification.generation++;
            display__unlock();
        }
        if (request.shutdown) {
            display__lock();
            s_worker_task = NULL;
            display__unlock();
            xSemaphoreGive(s_worker_stopped);
            vTaskDelete(NULL);
            continue;
        }
        display__lock();
        s_transfer_active = true;
        display__unlock();
#if CONFIG_BRUCE_QEMU_TEST_MODE
        display__lock();
        s_transfer_active = false;
        display__unlock();
        display__finish_request(&request, BRUCE_OK);
        continue;
#endif
        for (;;) {
            display__lock();
            bool conflict = request.overlay_update && display__overlay_conflicts_locked(request.rect);
            display__unlock();
            if (!conflict) break;
            vTaskDelay(1);
        }
        display__lock();
        notification__state_t notification = s_notification;
        bool compose = notification.active && display__rects_overlap(request.rect, notification.rect);
        bool packed = !request.fullscreen || compose || request.overlay_update;
        display__unlock();
        while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
        if (packed) {
            int x = request.rect.x;
            int y = request.rect.y;
            int w = request.rect.width;
            int h = request.rect.height;
            bool failed = false;
            for (int row = 0; row < h; ++row) {
                int screen_y = y + row;
                const bruce_display_color_t *pixels = &s_framebuffer[screen_y * s_fb_width + x];
                if (compose) {
                    display__lock();
                    memcpy(s_row_buffer, pixels, (size_t)w * sizeof(*pixels));
                    display__compose_notification_row(request.rect, &notification, screen_y);
                    display__unlock();
                    pixels = s_row_buffer;
                }
                if (display_driver__draw_bitmap(x, screen_y, x + w, screen_y + 1, pixels) != BRUCE_OK ||
                    xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                    failed = true;
                    break;
                }
            }
            display__lock();
            s_transfer_active = false;
            display__unlock();
            display__finish_request(&request, failed ? BRUCE_ERR_IO : BRUCE_OK);
            continue;
        }
        bruce_result_t result = display_driver__draw_bitmap(
            request.rect.x,
            request.rect.y,
            request.rect.x + request.rect.width,
            request.rect.y + request.rect.height,
            s_framebuffer
        );
        if (result == BRUCE_OK && xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) result = BRUCE_ERR_IO;
        display__lock();
        s_transfer_active = false;
        display__unlock();
        display__finish_request(&request, result);
    }
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
    if (s_worker_task != NULL) { vTaskDelete(s_worker_task); s_worker_task = NULL; }
    if (s_request_queue != NULL) { vQueueDelete(s_request_queue); s_request_queue = NULL; }
    if (s_transfer_done != NULL) { vSemaphoreDelete(s_transfer_done); s_transfer_done = NULL; }
    if (s_worker_stopped != NULL) { vSemaphoreDelete(s_worker_stopped); s_worker_stopped = NULL; }
    if (s_system_context.completion != NULL) {
        vSemaphoreDelete(s_system_context.completion);
        s_system_context.completion = NULL;
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].completion != NULL) {
            vSemaphoreDelete(s_contexts[i].completion);
            s_contexts[i].completion = NULL;
        }
    }
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
    s_framebuffer = heap_caps_malloc(DISPLAY__FB_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
    s_system_context.state = BRUCE_TASK_FOREGROUND;
    s_system_context.viewport = display__fullscreen_rect();
    s_system_context.completion = xSemaphoreCreateBinary();
    display__context_defaults(&s_system_context);
    s_transfer_done = xSemaphoreCreateBinary();
    s_worker_stopped = xSemaphoreCreateBinary();
    s_request_queue = xQueueCreate(DISPLAY__MAX_CONTEXTS + 1, sizeof(display__request_t));
    bool completion_failed = false;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].completion == NULL) {
            s_contexts[i].completion = xSemaphoreCreateBinary();
            if (s_contexts[i].completion == NULL) completion_failed = true;
        }
    }
    if (s_system_context.completion == NULL || s_transfer_done == NULL || s_worker_stopped == NULL ||
        s_request_queue == NULL || completion_failed ||
        xTaskCreate(
            display__worker, "bruce_display", DISPLAY__WORKER_STACK, NULL, tskIDLE_PRIORITY + 3, &s_worker_task
        ) != pdPASS) {
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
    QueueHandle_t queue = s_request_queue;
    SemaphoreHandle_t stopped = s_worker_stopped;
    TaskHandle_t worker = s_worker_task;
    display__unlock();
    if (worker != NULL && queue != NULL && stopped != NULL) {
        display__request_t shutdown = {.shutdown = true};
        if (xQueueSend(queue, &shutdown, portMAX_DELAY) == pdPASS) xSemaphoreTake(stopped, portMAX_DELAY);
    }
    display__lock();
    display__release_resources_locked();
    display__unlock();
}

int display__width(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    display__task_context_t *context = display__find_context_locked(caller);
    int width = context != NULL && !context->hidden ? context->viewport.width : 0;
    display__unlock();
    return width;
}

int display__height(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    display__task_context_t *context = display__find_context_locked(caller);
    int height = context != NULL && !context->hidden ? context->viewport.height : 0;
    display__unlock();
    return height;
}

bruce_result_t display__set_rotation(uint8_t rotation) {
    bruce_task_id_t caller_id = task__current_id();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    display__task_context_t *caller = display__find_context_locked(caller_id);
    if (caller == NULL || caller->tiled) { display__unlock(); return BRUCE_ERR_PERMISSION; }
    if (s_transfer_active || (s_request_queue != NULL && uxQueueMessagesWaiting(s_request_queue) != 0)) {
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
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
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    display__task_context_t *context = display__find_context_locked(caller);
    if (context == NULL) { display__unlock(); return BRUCE_ERR_PERMISSION; }
    if (context->completion == NULL) { display__unlock(); return BRUCE_ERR_NO_MEMORY; }
    if (context->frame_active) { display__unlock(); return BRUCE_ERR_INVALID_STATE; }
    context->frame_active = true;
    context->frame_noop = context->hidden;
    context->frame_generation = context->viewport_generation;
    if (!context->frame_noop) {
        for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
            display__task_context_t *other = &s_contexts[i];
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
    bruce_task_id_t caller = task__current_id();
    display__lock();
    display__task_context_t *context = display__find_context_locked(caller);
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    if (context == NULL || !context->frame_active) { display__unlock(); return BRUCE_ERR_INVALID_STATE; }
    if (context->frame_noop) {
        context->frame_active = false;
        context->frame_noop = false;
        display__unlock();
        return BRUCE_OK;
    }
    if (context->frame_generation != context->viewport_generation) {
        context->frame_active = false;
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    display__request_t request = {
        .context = context,
        .rect = context->viewport,
        .fullscreen = context->viewport.x == 0 && context->viewport.y == 0 &&
                      context->viewport.width == s_fb_width && context->viewport.height == s_fb_height,
    };
    while (xSemaphoreTake(context->completion, 0) == pdTRUE) {}
    context->transfer_pending = true;
    if (xQueueSend(s_request_queue, &request, 0) != pdPASS) {
        context->transfer_pending = false;
        context->frame_active = false;
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    display__unlock();
    if (xSemaphoreTake(context->completion, portMAX_DELAY) != pdTRUE) return BRUCE_ERR_IO;
    return context->completion_result;
}

bruce_result_t display__flush(void) {
    bruce_task_id_t caller = task__current_id();
    display__lock();
    display__task_context_t *context = display__find_context_locked(caller);
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
        task_registry__current_context(&built_in, NULL, 0, NULL) != BRUCE_OK || !built_in) {
        return BRUCE_ERR_PERMISSION;
    }
    bruce_task_id_t caller = task__current_id();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    display__task_context_t *owner = display__find_context_locked(caller);
    if (owner == NULL || owner->state != BRUCE_TASK_FOREGROUND) {
        display__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    display__task_context_t *targets[BRUCE_DISPLAY_MAX_TILES] = {0};
    for (size_t i = 0; i < count; ++i) {
        bruce_display_rect_t rect = tiles[i].rect;
        if (rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0 ||
            rect.x + rect.width > s_fb_width || rect.y + rect.height > s_fb_height) {
            display__unlock();
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        targets[i] = display__find_context_locked(tiles[i].task_id);
        if (targets[i] == NULL || !targets[i]->gui_requested || targets[i]->state != BRUCE_TASK_BACKGROUND) {
            display__unlock();
            return BRUCE_ERR_NOT_FOUND;
        }
        for (size_t j = 0; j < i; ++j) {
            if (tiles[j].task_id == tiles[i].task_id || display__rects_overlap(tiles[j].rect, rect)) {
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

void display__task_created(bruce_task_id_t task_id, bool gui_requested) {
    display__ensure_lock();
    display__lock();
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) {
            SemaphoreHandle_t completion = s_contexts[i].completion;
            memset(&s_contexts[i], 0, sizeof(s_contexts[i]));
            s_contexts[i].in_use = true;
            s_contexts[i].task_id = task_id;
            s_contexts[i].gui_requested = gui_requested;
            s_contexts[i].hidden = true;
            s_contexts[i].completion = completion != NULL ? completion : xSemaphoreCreateBinary();
            display__context_defaults(&s_contexts[i]);
            break;
        }
    }
    display__unlock();
}

void display__task_set_gui_requested(bruce_task_id_t task_id) {
    display__ensure_lock();
    display__lock();
    display__task_context_t *context = display__find_context_locked(task_id);
    if (context != NULL && !context->gui_requested) {
        context->gui_requested = true;
        if (context->state == BRUCE_TASK_FOREGROUND) context->clear_on_next_frame = true;
        if (!context->frame_active) display__set_visibility_locked(context);
    }
    display__unlock();
}

void display__task_state_changed(bruce_task_id_t task_id, bruce_task_state_t state) {
    display__ensure_lock();
    display__lock();
    display__task_context_t *context = display__find_context_locked(task_id);
    if (context != NULL) {
        if (context->gui_requested && state == BRUCE_TASK_FOREGROUND && context->state != BRUCE_TASK_FOREGROUND) {
            context->clear_on_next_frame = true;
        }
        context->state = state;
        if (state != BRUCE_TASK_FOREGROUND && context->frame_active && !context->tiled) context->frame_noop = true;
        if (!context->frame_active) display__set_visibility_locked(context);
    }
    display__unlock();
}

void display__task_removed(bruce_task_id_t task_id) {
    display__ensure_lock();
    display__lock();
    SemaphoreHandle_t completion = NULL;
    display__task_context_t *context = display__find_context_locked(task_id);
    if (context != NULL) {
        context->hidden = true;
        context->tiled = false;
        if (context->transfer_pending) context->remove_pending = true;
        else {
            context->frame_active = false;
            context->in_use = false;
            completion = context->completion;
            context->completion = NULL;
        }
    }
    display__unlock();
    if (completion != NULL) vSemaphoreDelete(completion);
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
    notification__state_t previous = s_notification;
    bruce_display_rect_t old_rect = s_notification.active ? s_notification.rect : (bruce_display_rect_t){0};
    strncpy(s_notification.text, text, sizeof(s_notification.text) - 1);
    s_notification.text[sizeof(s_notification.text) - 1] = '\0';
    s_notification.active = true;
    s_notification.duration_ms = duration_ms;
    s_notification.expires_at = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    s_notification.rect = display__notification_rect(s_notification.text);
    s_notification.generation++;
    display__request_t request = {
        .rect = display__rect_union(old_rect, s_notification.rect),
        .overlay_update = true,
        .notification_generation = s_notification.generation,
    };
    if (xQueueSend(s_request_queue, &request, 0) != pdPASS) {
        s_notification = previous;
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__notification_dismiss(void) {
    display__ensure_lock();
    display__lock();
    if (!s_initialized) { display__unlock(); return BRUCE_ERR_NOT_INITIALIZED; }
    if (!s_notification.active) { display__unlock(); return BRUCE_OK; }
    notification__state_t previous = s_notification;
    display__request_t request = {
        .rect = s_notification.rect,
        .overlay_update = true,
        .notification_generation = s_notification.generation,
    };
    s_notification.active = false;
    s_notification.generation++;
    if (xQueueSend(s_request_queue, &request, 0) != pdPASS) {
        s_notification = previous;
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    display__unlock();
    return BRUCE_OK;
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
