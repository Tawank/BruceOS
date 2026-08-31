#include "display.h"

#include <stdint.h>
#include <string.h>

#include "core/process/process.h"
#include "core_sdk/config.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "display_driver.h"
#include "display_internal.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "bruce_display"
#define DISPLAY__MAX_CONTEXTS 16
#define DISPLAY__MAX_RENDER_REQUESTS 8
#define DISPLAY__ROW_BUF_PIXELS                                                                              \
    (DISPLAY__NATIVE_WIDTH > DISPLAY__NATIVE_HEIGHT ? DISPLAY__NATIVE_WIDTH : DISPLAY__NATIVE_HEIGHT)

#define DISPLAY__DEFAULT_ROTATION CONFIG_BRUCE_DISPLAY_DEFAULT_ROTATION

/* Below this backlight byte value, the PWM duty cycle display_driver__set_backlight()
 * derives from it is too short to visibly light the panel, so a low-but-nonzero
 * brightness looks identical to the backlight being fully off. */
#define DISPLAY__MIN_VISIBLE_BRIGHTNESS 26

/* Short-lived structural lock: context/overlay table membership, viewport
 * and visibility transitions, rotation, tile layout, and frame-lease
 * bookkeeping. Never held across a draw primitive's pixel writes -- see
 * display__process_context_t.lock / display__overlay_t.surface.lock in
 * display_internal.h for what makes concurrent drawing across processes and
 * overlays safe without it. Kept recursive defensively; several call paths
 * below briefly release and re-acquire it around a DMA transfer while a
 * caller further up the same stack still expects to hold it. */
static SemaphoreHandle_t s_registry_mutex;
/* Serializes the actual panel DMA transfer (and its shared s_row_buffer /
 * s_transfer_done usage) across processes and against teardown, independent
 * of s_registry_mutex. display__transfer_locked() releases s_registry_mutex
 * for the duration of the transfer so other processes' draw calls are not
 * blocked while one process is presenting; this mutex keeps concurrent
 * transfers themselves (and display__deinit()) from racing on the single
 * physical display bus. */
static SemaphoreHandle_t s_transfer_mutex;
static bool s_initialized;
static bool s_buffered_rendering;
static bruce_display_color_t *s_framebuffer;
static bruce_display_color_t *s_direct_buffer;
/* Multi-row scratch buffer for display__transfer_locked()'s packed path
 * (buffered mode only, allocated alongside s_framebuffer) -- lets a
 * non-contiguous or overlay-composited partial rect go out as one DMA
 * transfer per several rows instead of one per row. Same pixel budget as
 * s_direct_buffer, whose allocation it never overlaps with (that one is
 * direct-mode-only), so peak RAM use does not increase. */
static bruce_display_color_t *s_pack_buffer;
static DMA_ATTR bruce_display_color_t s_row_buffer[DISPLAY__ROW_BUF_PIXELS];
static bool s_dma_framebuffer;
static uint8_t s_rotation;
static int16_t s_fb_width;
static int16_t s_fb_height;
static uint8_t s_brightness;
static display__process_context_t s_contexts[DISPLAY__MAX_CONTEXTS];
static display__process_context_t s_system_context;
static SemaphoreHandle_t s_transfer_done;
static bool s_dashboard_layout;
/* display__request_render_mode(): each live entry is one process's standing
 * request for a rendering mode leaner than the default. The display's actual
 * mode (see display__apply_effective_render_mode_locked()) is always the
 * leanest of these, or BRUCE_DISPLAY_MODE_BUFFERED_DMA if the table is empty
 * -- so several independent requesters compose correctly regardless of
 * order. display__process_removed() clears a process's entry if it exits (or
 * is killed) without releasing it itself. */
typedef struct {
    bool in_use;
    bruce_process_id_t process_id;
    bruce_display_render_mode_t mode;
} display__render_request_t;
static display__render_request_t s_render_requests[DISPLAY__MAX_RENDER_REQUESTS];

static inline void display__lock(void) { xSemaphoreTakeRecursive(s_registry_mutex, portMAX_DELAY); }
static inline void display__unlock(void) { xSemaphoreGiveRecursive(s_registry_mutex); }
static void display__ensure_lock(void);
/* bruce_reclaim_provider_t callbacks; registered with memory__reclaim() from
 * display__init(), defined near display__release_render_mode() below. */
static size_t display__reclaim_estimate(void);
static size_t display__reclaim_reclaim(void);
static void display__reclaim_restore(void);

void display_internal__lock_registry(void) { display__lock(); }
void display_internal__unlock_registry(void) { display__unlock(); }
bool display_internal__initialized(void) { return s_initialized; }
void display_internal__screen_size(int16_t *out_width, int16_t *out_height) {
    if (out_width != NULL) *out_width = s_fb_width;
    if (out_height != NULL) *out_height = s_fb_height;
}

bruce_result_t display__snapshot(
    uint16_t *pixels, size_t capacity, uint16_t *out_width, uint16_t *out_height, size_t *out_pixel_count
) {
    if (out_width == NULL || out_height == NULL || out_pixel_count == NULL ||
        (pixels == NULL && capacity != 0)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    display__ensure_lock();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    if (!s_buffered_rendering) {
        display__unlock();
        return BRUCE_ERR_UNSUPPORTED;
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
    if (s_registry_mutex == NULL) s_registry_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_transfer_mutex == NULL) s_transfer_mutex = xSemaphoreCreateMutex();
}

static bruce_display_rect_t display__fullscreen_rect(void) {
    return (bruce_display_rect_t){0, 0, s_fb_width, s_fb_height};
}

static void display__context_defaults(display__process_context_t *context) {
    context->text_color = BRUCE_COLOR_WHITE;
    context->text_bg_color = BRUCE_COLOR_BLACK;
    context->text_bg_transparent = false;
    context->text_size = 1.0f;
    context->cursor_x = 0;
    context->cursor_y = 0;
}

/* Caller must hold s_registry_mutex. */
static display__process_context_t *display__find_context_locked(bruce_process_id_t process_id) {
    if (process_id == BRUCE_PROCESS_ID_INVALID) return &s_system_context;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].process_id == process_id) return &s_contexts[i];
    }
    return NULL;
}

display__process_context_t *display_internal__find_context_locked(bruce_process_id_t process_id) {
    return display__find_context_locked(process_id);
}

static bool display__rects_overlap(bruce_display_rect_t a, bruce_display_rect_t b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

bruce_display_rect_t display_internal__rect_union(bruce_display_rect_t a, bruce_display_rect_t b) {
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

/* True if `rect` overlaps a live frame lease (the system context's
 * fullscreen frame, or any other context's active, non-noop tiled frame).
 * Used to avoid stepping on an in-flight frame with an immediate overlay
 * repaint -- the overlay simply appears on that frame's next transfer
 * instead. Caller must hold s_registry_mutex. */
static bool display__frame_conflicts_locked(bruce_display_rect_t rect) {
    if (s_system_context.frame_active && display__rects_overlap(s_system_context.viewport, rect)) return true;
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active && !s_contexts[i].frame_noop &&
            display__rects_overlap(s_contexts[i].viewport, rect)) {
            return true;
        }
    }
    return false;
}

/* Caller must hold s_registry_mutex; mutates only under `context->lock`
 * (never held together with another context's lock) so an in-progress draw
 * on `context` never observes a torn viewport/hidden pair. */
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
    if (context->lock != NULL) xSemaphoreTake(context->lock, portMAX_DELAY);
    if (context->hidden != hidden || memcmp(&context->viewport, &next, sizeof(next)) != 0) {
        context->viewport = next;
        context->hidden = hidden;
        context->viewport_generation++;
    }
    if (context->lock != NULL) xSemaphoreGive(context->lock);
}

bruce_result_t display_internal__begin_draw(display__process_context_t **out_context) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__process_context_t *context = display__find_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    display__process_context_t *target =
        context->active_overlay != NULL ? &context->active_overlay->surface : context;
    display__unlock();
    xSemaphoreTake(target->lock, portMAX_DELAY);
    if (out_context != NULL) *out_context = target;
    return BRUCE_OK;
}

void display_internal__unlock(display__process_context_t *context) { xSemaphoreGive(context->lock); }

void display_internal__set_pixel(
    display__process_context_t *context, int16_t x, int16_t y, bruce_display_color_t color
) {
    if (context == NULL || context->hidden || x < 0 || x >= context->viewport.width || y < 0 ||
        y >= context->viewport.height) {
        return;
    }
    int screen_x = context->viewport.x + x;
    int screen_y = context->viewport.y + y;
    if (context->target_buffer != NULL) {
        context->target_buffer[screen_y * context->target_stride + screen_x] = color;
        return;
    }
    if (screen_x >= 0 && screen_x < s_fb_width && screen_y >= 0 && screen_y < s_fb_height) {
        if (s_buffered_rendering) {
            s_framebuffer[screen_y * s_fb_width + screen_x] = color;
        } else {
#if !CONFIG_BRUCE_QEMU_TEST_MODE
            xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
            while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
            s_direct_buffer[0] = color;
            bruce_result_t result =
                display_driver__draw_bitmap(screen_x, screen_y, screen_x + 1, screen_y + 1, s_direct_buffer);
            if (result == BRUCE_OK && xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                result = BRUCE_ERR_IO;
            }
            xSemaphoreGive(s_transfer_mutex);
            if (context->draw_result == BRUCE_OK && result != BRUCE_OK) context->draw_result = result;
#endif
        }
    }
}

void display_internal__fill_rect(
    display__process_context_t *context, int16_t x, int16_t y, int16_t w, int16_t h,
    bruce_display_color_t color
) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (context == NULL || context->hidden) return;
    if (x + w > context->viewport.width) w = context->viewport.width - x;
    if (y + h > context->viewport.height) h = context->viewport.height - y;
    if (w <= 0 || h <= 0) return;
    x += context->viewport.x;
    y += context->viewport.y;
    if (context->target_buffer != NULL) {
        for (int16_t row = y; row < y + h; ++row) {
            bruce_display_color_t *pixels = &context->target_buffer[row * context->target_stride + x];
            for (int16_t col = 0; col < w; ++col) pixels[col] = color;
        }
        return;
    }
    if (!s_buffered_rendering) {
#if !CONFIG_BRUCE_QEMU_TEST_MODE
        xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
        int16_t rows_per_transfer = DISPLAY__DIRECT_BUF_PIXELS / w;
        if (rows_per_transfer > h) rows_per_transfer = h;
        size_t transfer_pixels = (size_t)w * (size_t)rows_per_transfer;
        for (size_t pixel = 0; pixel < transfer_pixels; ++pixel) s_direct_buffer[pixel] = color;
        for (int16_t row = y; row < y + h; row += rows_per_transfer) {
            int16_t rows = y + h - row;
            if (rows > rows_per_transfer) rows = rows_per_transfer;
            while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
            bruce_result_t result = display_driver__draw_bitmap(x, row, x + w, row + rows, s_direct_buffer);
            if (result == BRUCE_OK && xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                result = BRUCE_ERR_IO;
            }
            if (result != BRUCE_OK) {
                if (context->draw_result == BRUCE_OK) context->draw_result = result;
                break;
            }
        }
        xSemaphoreGive(s_transfer_mutex);
#endif
        return;
    }
    for (int16_t row = y; row < y + h; ++row) {
        bruce_display_color_t *pixels = &s_framebuffer[row * s_fb_width + x];
        for (int16_t col = 0; col < w; ++col) pixels[col] = color;
    }
}

void display_internal__draw_rgb_bitmap(
    display__process_context_t *context, int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h
) {
    if (context == NULL || context->hidden) return;
    int16_t source_stride = w;
    int16_t src_x = 0;
    int16_t src_y = 0;
    if (x < 0) {
        src_x = -x;
        w += x;
        x = 0;
    }
    if (y < 0) {
        src_y = -y;
        h += y;
        y = 0;
    }
    if (x + w > context->viewport.width) w = context->viewport.width - x;
    if (y + h > context->viewport.height) h = context->viewport.height - y;
    if (w <= 0 || h <= 0) return;
    int16_t screen_x = x + context->viewport.x;
    int16_t screen_y = y + context->viewport.y;
    if (context->target_buffer != NULL) {
        for (int16_t row = 0; row < h; ++row) {
            memcpy(
                &context->target_buffer[(screen_y + row) * context->target_stride + screen_x],
                &bitmap[(src_y + row) * source_stride + src_x],
                (size_t)w * sizeof(*bitmap)
            );
        }
        return;
    }
    if (s_buffered_rendering) {
        for (int16_t row = 0; row < h; ++row) {
            memcpy(
                &s_framebuffer[(screen_y + row) * s_fb_width + screen_x],
                &bitmap[(src_y + row) * source_stride + src_x],
                (size_t)w * sizeof(*bitmap)
            );
        }
        return;
    }
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    /* Ping-pong s_direct_buffer's two halves. The panel queue accepts both
     * halves, so packing and submitting the second chunk doesn't wait for
     * the first transfer. We wait only when reusing a half, after its earlier
     * transfer has completed. */
    int16_t rows_per_transfer = DISPLAY__DIRECT_CHUNK_PIXELS / w;
    if (rows_per_transfer > h) rows_per_transfer = h;
    if (rows_per_transfer < 1) rows_per_transfer = 1;
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
    bruce_result_t result = BRUCE_OK;
    bool pending[2] = {false, false};
    int buf_half = 0;
    for (int16_t row = 0; row < h; row += rows_per_transfer) {
        int16_t rows = h - row;
        if (rows > rows_per_transfer) rows = rows_per_transfer;
        if (pending[buf_half]) {
            pending[buf_half] = false;
            int64_t dma_wait_start = esp_timer_get_time();
            if (xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                result = BRUCE_ERR_IO;
                break;
            }
            int64_t dma_wait_us = esp_timer_get_time() - dma_wait_start;
            if (dma_wait_us > 10000) {
                ESP_LOGW(TAG, "direct display DMA completion took %lld us", (long long)dma_wait_us);
            }
        }
        bruce_display_color_t *buf = s_direct_buffer + (buf_half == 0 ? 0 : DISPLAY__DIRECT_CHUNK_PIXELS);
        for (int16_t copy_row = 0; copy_row < rows; ++copy_row) {
            memcpy(
                &buf[(size_t)copy_row * (size_t)w],
                &bitmap[(src_y + row + copy_row) * source_stride + src_x],
                (size_t)w * sizeof(*bitmap)
            );
        }
        result =
            display_driver__draw_bitmap(screen_x, screen_y + row, screen_x + w, screen_y + row + rows, buf);
        if (result != BRUCE_OK) break;
        pending[buf_half] = true;
        buf_half ^= 1;
    }
    for (int half = 0; half < 2; ++half) {
        if (!pending[half]) continue;
        int64_t dma_wait_start = esp_timer_get_time();
        if (xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE && result == BRUCE_OK)
            result = BRUCE_ERR_IO;
        int64_t dma_wait_us = esp_timer_get_time() - dma_wait_start;
        if (dma_wait_us > 10000) {
            ESP_LOGW(TAG, "direct display DMA completion took %lld us", (long long)dma_wait_us);
        }
    }
    xSemaphoreGive(s_transfer_mutex);
    if (result != BRUCE_OK && context->draw_result == BRUCE_OK) context->draw_result = result;
#endif
}

IRAM_ATTR bool display_internal__on_transfer_done_from_isr(void) {
    BaseType_t high_priority_woken = pdFALSE;
    if (s_transfer_done != NULL) xSemaphoreGiveFromISR(s_transfer_done, &high_priority_woken);
    return high_priority_woken == pdTRUE;
}

/* Pushes `rect` from the framebuffer to the panel, compositing overlays
 * into the transferred rows.  Called with the display lock held (as its
 * "_locked" name implies).
 *
 * When no visible overlay intersects `rect` (the common case), the transfer
 * touches only the framebuffer and s_pack_buffer, so the registry lock is
 * released for the duration of the actual DMA transfer and other processes'
 * draw calls are not blocked while this one is presenting; it is reacquired
 * before returning.
 *
 * When an overlay does intersect `rect`, composition
 * (display_overlay__compose_row_locked()) reads the live s_overlays[] table
 * and each overlay's separately heap-allocated pixel buffer directly --
 * unlike the old single fixed-size notification struct this replaced, there
 * is no cheap self-contained snapshot to take before releasing the lock, so
 * the registry lock stays held for this transfer's whole duration instead:
 * releasing it here would let a concurrent display__overlay_destroy() (or
 * _move()) on another task free or resize that buffer while this loop is
 * still reading it. This only blocks other processes' draws while an
 * overlay-covered region -- typically a small notification banner -- is
 * being presented, not on every frame.
 *
 * The non-fullscreen "packed" path below (see its own comment) chunks
 * several rows per DMA transfer instead of one, via s_pack_buffer, whenever
 * rows can't be read straight out of s_framebuffer in one contiguous block.
 *
 * The transfer itself is also serialized -- against concurrent transfers
 * from other processes, and against display__deinit() freeing the
 * framebuffer/pack buffer out from under it -- via s_transfer_mutex, since
 * there is only one physical display bus and one shared pack buffer. */
static bruce_result_t display__transfer_locked(bruce_display_rect_t rect, bool fullscreen) {
#if CONFIG_BRUCE_QEMU_TEST_MODE
    (void)rect;
    (void)fullscreen;
    return BRUCE_OK;
#else
    bool compose = display_overlay__intersects_locked(rect);
    bool packed = !s_dma_framebuffer || !fullscreen || compose;

    if (!compose) display__unlock();
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);

    bruce_result_t result = BRUCE_OK;
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
    if (packed) {
        bool needs_copy = compose || !s_dma_framebuffer;
        /* When the framebuffer itself is DMA-capable and nothing is being
         * composited over it, a row's pixels don't need to move through any
         * scratch buffer at all -- and if `rect` is also full-width, its
         * rows are contiguous in s_framebuffer (row N's last pixel is
         * immediately followed by row N+1's first), so the whole remaining
         * block can go out as a single transfer, exactly like the
         * already-fullscreen case below. Only a genuinely narrower rect (or
         * one needing composition/copy) is chunked through s_pack_buffer,
         * several rows at a time, to cut DMA round trips per row down to
         * one per chunk. */
        int chunk_rows = 1;
        if (!needs_copy) {
            if (rect.width == s_fb_width) chunk_rows = rect.height;
        } else {
            chunk_rows = (int)(DISPLAY__DIRECT_BUF_PIXELS / rect.width);
            if (chunk_rows < 1) chunk_rows = 1;
        }
        for (int row = 0; row < rect.height;) {
            int screen_y = rect.y + row;
            int rows = rect.height - row;
            if (rows > chunk_rows) rows = chunk_rows;
            const bruce_display_color_t *pixels = &s_framebuffer[screen_y * s_fb_width + rect.x];
            if (needs_copy) {
                for (int r = 0; r < rows; ++r) {
                    bruce_display_color_t *dst = &s_pack_buffer[(size_t)r * rect.width];
                    memcpy(
                        dst,
                        &s_framebuffer[(size_t)(screen_y + r) * s_fb_width + rect.x],
                        (size_t)rect.width * sizeof(*dst)
                    );
                    if (compose) display_overlay__compose_row_locked(rect, screen_y + r, dst);
                }
                pixels = s_pack_buffer;
            }
            if (display_driver__draw_bitmap(rect.x, screen_y, rect.x + rect.width, screen_y + rows, pixels) !=
                    BRUCE_OK ||
                xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE) {
                result = BRUCE_ERR_IO;
                break;
            }
            row += rows;
        }
    } else {
        result = display_driver__draw_bitmap(
            rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, s_framebuffer
        );
        if (result == BRUCE_OK && xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE)
            result = BRUCE_ERR_IO;
    }

    xSemaphoreGive(s_transfer_mutex);
    if (!compose) display__lock();
    return result;
#endif
}

bruce_result_t display_internal__repaint_rect_locked(bruce_display_rect_t rect) {
    if (display__frame_conflicts_locked(rect)) return BRUCE_OK;
    if (s_buffered_rendering) return display__transfer_locked(rect, false);
    return display_overlay__paint_direct_locked(rect);
}

bruce_result_t display_internal__stream_row_locked(
    int16_t x, int16_t y, int16_t width, const bruce_display_color_t *pixels
) {
#if CONFIG_BRUCE_QEMU_TEST_MODE
    (void)x;
    (void)y;
    (void)width;
    (void)pixels;
    return BRUCE_OK;
#else
    if (width <= 0 || width > DISPLAY__ROW_BUF_PIXELS) return BRUCE_ERR_INVALID_ARGUMENT;
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    memcpy(s_row_buffer, pixels, (size_t)width * sizeof(*pixels));
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
    bruce_result_t result = display_driver__draw_bitmap(x, y, x + width, y + 1, s_row_buffer) != BRUCE_OK ||
                                    xSemaphoreTake(s_transfer_done, portMAX_DELAY) != pdTRUE
                                ? BRUCE_ERR_IO
                                : BRUCE_OK;
    xSemaphoreGive(s_transfer_mutex);
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
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    display_driver__configure_rotation(s_rotation);
    xSemaphoreGive(s_transfer_mutex);
}

static void display__release_resources_locked(void) {
    /* display__transfer_locked() runs its actual DMA transfer with
     * s_registry_mutex released, holding only s_transfer_mutex.
     * s_initialized is already false by the time callers reach here (see
     * display__deinit()), so no new transfer can start, but one already in
     * flight must be allowed to finish before we free the buffers/semaphore
     * it's using. */
    if (s_transfer_mutex != NULL) xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    if (s_transfer_done != NULL) {
        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;
    }
    if (s_direct_buffer != NULL) {
        heap_caps_free(s_direct_buffer);
        s_direct_buffer = NULL;
    }
    if (s_pack_buffer != NULL) {
        heap_caps_free(s_pack_buffer);
        s_pack_buffer = NULL;
    }
    if (s_framebuffer != NULL) {
        heap_caps_free(s_framebuffer);
        s_framebuffer = NULL;
    }
    display_driver__deinit();
    if (s_transfer_mutex != NULL) xSemaphoreGive(s_transfer_mutex);
}

/* Switches the live rendering mode between buffered (with or without a
 * DMA-capable framebuffer) and direct, at runtime -- the same allocation
 * display__init() does up front, just replayed on demand. Used by
 * display__apply_effective_render_mode_locked() to free (or restore) the
 * off-screen framebuffer without a reboot.
 *
 * The new buffer(s) are allocated before any old one is freed, so a
 * transient allocation failure leaves the previous mode fully intact instead
 * of tearing display rendering down; only on success are the old buffers
 * replaced. Caller must hold the registry lock, with no frame currently
 * active on any context -- display__apply_effective_render_mode_locked()
 * enforces both -- so no new draw or transfer can start against the buffers
 * being swapped out here. */
static bruce_result_t display__reconfigure_render_mode_locked(bool buffered, bool dma) {
    bruce_display_color_t *new_framebuffer = NULL;
    bruce_display_color_t *new_pack_buffer = NULL;
    bruce_display_color_t *new_direct_buffer = NULL;
    bool new_dma = buffered && dma;

    if (buffered) {
        uint32_t framebuffer_caps = MALLOC_CAP_INTERNAL | (new_dma ? MALLOC_CAP_DMA : MALLOC_CAP_8BIT);
        new_framebuffer = heap_caps_malloc(DISPLAY__FB_SIZE, framebuffer_caps);
        if (new_framebuffer == NULL) return BRUCE_ERR_NO_MEMORY;
        new_pack_buffer = heap_caps_malloc(
            DISPLAY__DIRECT_BUF_PIXELS * sizeof(*new_pack_buffer), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
        );
        if (new_pack_buffer == NULL) {
            heap_caps_free(new_framebuffer);
            return BRUCE_ERR_NO_MEMORY;
        }
        memset(new_framebuffer, 0, DISPLAY__FB_SIZE);
    } else {
        new_direct_buffer = heap_caps_malloc(
            DISPLAY__DIRECT_BUF_PIXELS * sizeof(*new_direct_buffer), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
        );
        if (new_direct_buffer == NULL) return BRUCE_ERR_NO_MEMORY;
    }

    /* Mirrors display__release_resources_locked(): the old buffers are what
     * an in-flight transfer (running with s_registry_mutex released, see
     * display__transfer_locked()) is using, so freeing them is serialized
     * against it via s_transfer_mutex rather than the registry lock alone. */
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {}
    if (s_framebuffer != NULL) heap_caps_free(s_framebuffer);
    if (s_pack_buffer != NULL) heap_caps_free(s_pack_buffer);
    if (s_direct_buffer != NULL) heap_caps_free(s_direct_buffer);
    s_framebuffer = new_framebuffer;
    s_pack_buffer = new_pack_buffer;
    s_direct_buffer = new_direct_buffer;
    s_buffered_rendering = buffered;
    s_dma_framebuffer = new_dma;
    xSemaphoreGive(s_transfer_mutex);
    return BRUCE_OK;
}

static bruce_display_render_mode_t display__current_render_mode_locked(void) {
    if (!s_buffered_rendering) return BRUCE_DISPLAY_MODE_DIRECT;
    return s_dma_framebuffer ? BRUCE_DISPLAY_MODE_BUFFERED_DMA : BRUCE_DISPLAY_MODE_BUFFERED;
}

/* Recomputes the leanest mode any live entry in s_render_requests[] asks
 * for, and applies it if that differs from the display's current mode.
 * Caller must hold the registry lock, with no frame active on any context
 * (display__request_render_mode()/display__release_render_mode() enforce
 * this) so display__reconfigure_render_mode_locked() never swaps buffers out
 * from under an in-progress draw or transfer. */
static bruce_result_t display__apply_effective_render_mode_locked(void) {
    bruce_display_render_mode_t target = BRUCE_DISPLAY_MODE_BUFFERED_DMA;
    for (size_t i = 0; i < DISPLAY__MAX_RENDER_REQUESTS; ++i) {
        if (s_render_requests[i].in_use && s_render_requests[i].mode > target) target = s_render_requests[i].mode;
    }
    if (target == display__current_render_mode_locked()) return BRUCE_OK;
    return display__reconfigure_render_mode_locked(
        target != BRUCE_DISPLAY_MODE_DIRECT, target == BRUCE_DISPLAY_MODE_BUFFERED_DMA
    );
}

/* Creates `context->lock` on first use and preserves it across a memset of
 * the rest of the slot, so the fixed-size context/overlay tables never
 * churn FreeRTOS semaphore handles across process create/remove cycles. */
static bool display__reset_context_slot(display__process_context_t *context) {
    SemaphoreHandle_t lock = context->lock;
    if (lock == NULL) lock = xSemaphoreCreateMutex();
    if (lock == NULL) return false;
    memset(context, 0, sizeof(*context));
    context->lock = lock;
    return true;
}

bruce_result_t display__init(void) {
    display__ensure_lock();
    display__lock();
    if (s_initialized) {
        display__unlock();
        return BRUCE_OK;
    }
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    bruce_result_t result = display_driver__init();
    if (result != BRUCE_OK) {
        display__unlock();
        return result;
    }
#endif
    s_buffered_rendering = config__get_display_buffered_rendering();
    s_dma_framebuffer = s_buffered_rendering && config__get_display_dma_framebuffer();
    if (s_buffered_rendering) {
        uint32_t framebuffer_caps =
            MALLOC_CAP_INTERNAL | (s_dma_framebuffer ? MALLOC_CAP_DMA : MALLOC_CAP_8BIT);
        s_framebuffer = heap_caps_malloc(DISPLAY__FB_SIZE, framebuffer_caps);
        if (s_framebuffer == NULL) {
            ESP_LOGE(TAG, "failed to allocate framebuffer");
            display_driver__deinit();
            display__unlock();
            return BRUCE_ERR_NO_MEMORY;
        }
        s_pack_buffer = heap_caps_malloc(
            DISPLAY__DIRECT_BUF_PIXELS * sizeof(*s_pack_buffer), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
        );
        if (s_pack_buffer == NULL) {
            ESP_LOGE(TAG, "failed to allocate transfer pack buffer");
            display__release_resources_locked();
            display__unlock();
            return BRUCE_ERR_NO_MEMORY;
        }
    } else {
        s_direct_buffer = heap_caps_malloc(
            DISPLAY__DIRECT_BUF_PIXELS * sizeof(*s_direct_buffer), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
        );
        if (s_direct_buffer == NULL) {
            ESP_LOGE(TAG, "failed to allocate direct rendering buffer");
            display_driver__deinit();
            display__unlock();
            return BRUCE_ERR_NO_MEMORY;
        }
    }
    s_rotation = DISPLAY__DEFAULT_ROTATION;
    display__configure_rotation();
    if (!display__reset_context_slot(&s_system_context)) {
        display__release_resources_locked();
        display__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    s_system_context.in_use = true;
    s_system_context.gui_requested = true;
    s_system_context.state = BRUCE_PROCESS_FOREGROUND;
    s_system_context.viewport = display__fullscreen_rect();
    display__context_defaults(&s_system_context);
    /* Direct mode can have two panel transfers queued at once. A counting
     * semaphore preserves both completion notifications if they arrive before
     * the renderer next needs either DMA buffer. */
    s_transfer_done = xSemaphoreCreateCounting(2, 0);
    if (s_transfer_done == NULL) {
        display__release_resources_locked();
        display__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use) display__set_visibility_locked(&s_contexts[i]);
    }
    if (s_framebuffer != NULL) memset(s_framebuffer, 0, DISPLAY__FB_SIZE);
    int configured_brightness = config__get_display_brightness();
    s_brightness = (uint8_t)((configured_brightness * 255) / 100);
    if (configured_brightness > 0 && s_brightness < DISPLAY__MIN_VISIBLE_BRIGHTNESS)
        s_brightness = DISPLAY__MIN_VISIBLE_BRIGHTNESS;
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    display_driver__set_backlight(s_brightness);
#endif
    s_initialized = true;
    display__unlock();
    static bool s_reclaim_provider_registered;
    if (!s_reclaim_provider_registered) {
        static const bruce_reclaim_provider_t reclaim_provider = {
            .name = "display",
            .estimate = display__reclaim_estimate,
            .reclaim = display__reclaim_reclaim,
            .restore = display__reclaim_restore,
        };
        s_reclaim_provider_registered = memory__register_reclaimable(&reclaim_provider) == BRUCE_OK;
    }
    if (s_buffered_rendering) return display__flush();
    bruce_result_t direct_result = display__begin_frame();
    if (direct_result == BRUCE_OK) direct_result = display__fill_screen(BRUCE_COLOR_BLACK);
    if (direct_result == BRUCE_OK) direct_result = display__present();
    if (direct_result != BRUCE_OK) display__deinit();
    return direct_result;
}

void display__deinit(void) {
    if (s_registry_mutex == NULL) return;
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return;
    }
    s_initialized = false;
    /* display__release_resources_locked() below frees whichever buffers are
     * currently live regardless of any standing render-mode requests; clear
     * the table so a later display__init() doesn't come up leaning on
     * requests from processes it no longer knows about. */
    memset(s_render_requests, 0, sizeof(s_render_requests));
    display_overlay__deinit();
    display__release_resources_locked();
    display__unlock();
}

int display__width(void) {
    bruce_process_id_t caller = process__current_id();
    /* A process's viewport isn't sized until it's promoted to a real GUI
     * surface (see process_registry__mark_presentable(); display__begin_frame()
     * triggers the same promotion for the same reason). Without this, a
     * script that asks for its own width/height before its first draw -- the
     * natural place to size sprite layout, e.g. dino_game.js's startup code
     * -- would read back a stale (0, 0) from a context that is still hidden,
     * even though it's the sole foreground process and about to be given the
     * whole screen. Callers that want the physical panel size without this
     * side effect already have display__screen_width()/height() for that. */
    process_registry__mark_presentable(caller);
    display__lock();
    display__process_context_t *context = display__find_context_locked(caller);
    if (context != NULL && context->active_overlay != NULL) context = &context->active_overlay->surface;
    int width = context != NULL && !context->hidden ? context->viewport.width : 0;
    display__unlock();
    return width;
}

int display__height(void) {
    bruce_process_id_t caller = process__current_id();
    /* See display__width() -- promotes the caller to a real GUI surface
     * before reading its viewport, for the same reason display__begin_frame()
     * does. */
    process_registry__mark_presentable(caller);
    display__lock();
    display__process_context_t *context = display__find_context_locked(caller);
    if (context != NULL && context->active_overlay != NULL) context = &context->active_overlay->surface;
    int height = context != NULL && !context->hidden ? context->viewport.height : 0;
    display__unlock();
    return height;
}

int display__screen_width(void) {
    display__lock();
    int width = s_initialized ? s_fb_width : 0;
    display__unlock();
    return width;
}

int display__screen_height(void) {
    display__lock();
    int height = s_initialized ? s_fb_height : 0;
    display__unlock();
    return height;
}

bruce_result_t display__set_rotation(uint8_t rotation) {
    bruce_process_id_t caller_id = process__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__process_context_t *caller = display__find_context_locked(caller_id);
    if (caller == NULL || caller->tiled) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active) {
            display__unlock();
            return BRUCE_ERR_BUSY;
        }
    }
    if (s_system_context.frame_active) {
        display__unlock();
        return BRUCE_ERR_BUSY;
    }
    s_rotation = rotation & 3;
    display__configure_rotation();
    /* Overlay rects are absolute screen coordinates chosen by their owner
     * and are not automatically re-anchored here (no rotation-change event
     * is exposed to apps either). An owner whose overlay would fall
     * partially off-screen after a rotation change is responsible for
     * calling display__overlay_move() itself; compositing an out-of-bounds
     * rect is safe either way -- see display_overlay__compose_row_locked()
     * and display_overlay__paint_direct_locked(), which only ever touch
     * memory within the intersection of the overlay's own buffer and the
     * (always screen-bounded) transfer/clip rect. */
    if (s_framebuffer != NULL) memset(s_framebuffer, 0, DISPLAY__FB_SIZE);
    if (s_system_context.lock != NULL) xSemaphoreTake(s_system_context.lock, portMAX_DELAY);
    s_system_context.viewport = display__fullscreen_rect();
    s_system_context.viewport_generation++;
    if (s_system_context.lock != NULL) xSemaphoreGive(s_system_context.lock);
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
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
#if CONFIG_BRUCE_QEMU_TEST_MODE
    bruce_result_t result = BRUCE_OK;
#else
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    (void)display_driver__invert(invert);
    xSemaphoreGive(s_transfer_mutex);
    bruce_result_t result = BRUCE_OK;
#endif
    display__unlock();
    return result;
}

bruce_result_t display__set_brightness(uint8_t brightness) {
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__unlock();
    /* Floor any nonzero request to a visibly-lit minimum; 0 still turns the
     * backlight fully off. See DISPLAY__MIN_VISIBLE_BRIGHTNESS above. */
    if (brightness > 0 && brightness < DISPLAY__MIN_VISIBLE_BRIGHTNESS)
        brightness = DISPLAY__MIN_VISIBLE_BRIGHTNESS;
    bruce_result_t result = config__set_display_brightness((int)brightness * 100 / 255);
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
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
#if CONFIG_BRUCE_QEMU_TEST_MODE
    bruce_result_t result = BRUCE_OK;
#else
    xSemaphoreTake(s_transfer_mutex, portMAX_DELAY);
    (void)display_driver__set_enabled(on);
    xSemaphoreGive(s_transfer_mutex);
    bruce_result_t result = BRUCE_OK;
#endif
    display__unlock();
    return result;
}

/* Caller must hold the registry lock. Finds the calling process's existing
 * entry in s_render_requests[], if any, so display__request_render_mode()
 * and display__release_render_mode() share one lookup. */
static display__render_request_t *display__find_render_request_locked(bruce_process_id_t process_id) {
    for (size_t i = 0; i < DISPLAY__MAX_RENDER_REQUESTS; ++i) {
        if (s_render_requests[i].in_use && s_render_requests[i].process_id == process_id) {
            return &s_render_requests[i];
        }
    }
    return NULL;
}

/* Caller must hold the registry lock. No frame may be active on any context
 * -- checked here, alongside display__apply_effective_render_mode_locked()'s
 * own actual mode change, since either failure must leave s_render_requests[]
 * exactly as it was found. */
static bruce_result_t display__frame_active_locked(void) {
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (s_contexts[i].in_use && s_contexts[i].frame_active) return BRUCE_ERR_BUSY;
    }
    return s_system_context.frame_active ? BRUCE_ERR_BUSY : BRUCE_OK;
}

bruce_result_t display__request_render_mode(bruce_display_render_mode_t mode) {
    if (mode < BRUCE_DISPLAY_MODE_BUFFERED_DMA || mode > BRUCE_DISPLAY_MODE_DIRECT) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_process_id_t caller = process__current_id();
    /* BRUCE_PROCESS_ID_INVALID means "no process" -- a request table entry
     * for it would be meaningless (nothing to release it on exit) and would
     * collide with any other caller with no process, so reject it up front,
     * same as the old display__game_mode() did. */
    if (caller == BRUCE_PROCESS_ID_INVALID) return BRUCE_ERR_PERMISSION;
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__render_request_t *slot = display__find_render_request_locked(caller);
    if (slot == NULL && mode == BRUCE_DISPLAY_MODE_BUFFERED_DMA) {
        /* Requesting the default with no request on file is a no-op, same as
         * display__release_render_mode() with nothing to release. */
        display__unlock();
        return BRUCE_OK;
    }
    if (slot == NULL) {
        for (size_t i = 0; i < DISPLAY__MAX_RENDER_REQUESTS; ++i) {
            if (!s_render_requests[i].in_use) {
                slot = &s_render_requests[i];
                break;
            }
        }
        if (slot == NULL) {
            display__unlock();
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
    }
    bruce_display_render_mode_t previous_mode = slot->in_use ? slot->mode : BRUCE_DISPLAY_MODE_BUFFERED_DMA;
    bool was_in_use = slot->in_use;
    slot->in_use = mode != BRUCE_DISPLAY_MODE_BUFFERED_DMA;
    slot->process_id = caller;
    slot->mode = mode;
    bruce_result_t result = display__frame_active_locked();
    if (result == BRUCE_OK) result = display__apply_effective_render_mode_locked();
    if (result != BRUCE_OK) {
        /* Leave the table exactly as found on failure, so a rejected request
         * can't silently change what the caller already held. */
        slot->in_use = was_in_use;
        slot->mode = previous_mode;
    }
    display__unlock();
    return result;
}

bruce_result_t display__release_render_mode(void) {
    bruce_process_id_t caller = process__current_id();
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__render_request_t *slot = display__find_render_request_locked(caller);
    if (slot == NULL) {
        display__unlock();
        return BRUCE_OK;
    }
    bruce_result_t result = display__frame_active_locked();
    if (result == BRUCE_OK) {
        slot->in_use = false;
        result = display__apply_effective_render_mode_locked();
        if (result != BRUCE_OK) slot->in_use = true;
    }
    display__unlock();
    return result;
}

/* Caller must hold the registry lock. Clears any render-mode request
 * belonging to `process_id` and reapplies the resulting effective mode,
 * unconditionally -- unlike display__release_render_mode(), this skips the
 * frame-active check, since process teardown itself may leave `process_id`'s
 * own context (or another one) mid-frame, and a process that exited (or was
 * killed) without releasing its own request first must not leave the
 * display stuck leaner than every remaining live request needs, forever. */
static void display__release_render_mode_for_process_locked(bruce_process_id_t process_id) {
    display__render_request_t *slot = display__find_render_request_locked(process_id);
    if (slot == NULL) return;
    slot->in_use = false;
    bruce_result_t result = display__apply_effective_render_mode_locked();
    if (result != BRUCE_OK) {
        ESP_LOGE(TAG, "display: failed to restore rendering mode for exited process (%d)", (int)result);
    }
}

size_t display__buffer_footprint(void) {
    display__lock();
    size_t bytes = 0;
    if (s_initialized) {
        size_t direct_bytes = DISPLAY__DIRECT_BUF_PIXELS * sizeof(bruce_display_color_t);
        bytes = s_buffered_rendering ? (size_t)DISPLAY__FB_SIZE + direct_bytes : direct_bytes;
    }
    display__unlock();
    return bytes;
}

/* bruce_reclaim_provider_t callbacks (registered with memory__reclaim() in
 * display__init()): reaching BRUCE_DISPLAY_MODE_DIRECT frees the off-screen
 * framebuffer and its pack buffer, which together cost exactly
 * DISPLAY__FB_SIZE more than the direct-mode scratch buffer that replaces
 * them (same size as the pack buffer it takes over from) -- see
 * display__buffer_footprint(). Dropping DMA capability alone
 * (BRUCE_DISPLAY_MODE_BUFFERED) frees no bytes, just changes which RAM pool
 * the framebuffer draws from, so it is not a reclaim step. */
static size_t display__reclaim_estimate(void) {
    display__lock();
    size_t bytes =
        (s_initialized && display__current_render_mode_locked() != BRUCE_DISPLAY_MODE_DIRECT)
            ? DISPLAY__FB_SIZE
            : 0;
    display__unlock();
    return bytes;
}

static size_t display__reclaim_reclaim(void) {
    size_t estimate = display__reclaim_estimate();
    if (estimate == 0) return 0;
    return display__request_render_mode(BRUCE_DISPLAY_MODE_DIRECT) == BRUCE_OK ? estimate : 0;
}

static void display__reclaim_restore(void) { (void)display__release_render_mode(); }

bruce_result_t display__begin_frame(void) {
    bruce_process_id_t caller = process__current_id();
    process_registry__mark_presentable(caller);
    display__lock();
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__process_context_t *context = display__find_context_locked(caller);
    if (context == NULL) {
        display__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    if (context->frame_active) {
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
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
            display_internal__fill_rect(
                context, 0, 0, context->viewport.width, context->viewport.height, BRUCE_COLOR_BLACK
            );
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
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (context == NULL || !context->frame_active) {
        display__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
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
    bruce_result_t result;
    if (s_buffered_rendering) {
        result = display__transfer_locked(context->viewport, fullscreen);
    } else {
        result = context->draw_result;
        context->draw_result = BRUCE_OK;
        bruce_result_t composite_result = display_overlay__paint_direct_locked(context->viewport);
        if (result == BRUCE_OK) result = composite_result;
    }
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
    if (!s_initialized) {
        display__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
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
        if (targets[i] == NULL || !targets[i]->gui_requested ||
            targets[i]->state != BRUCE_PROCESS_BACKGROUND) {
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
        if (targets[i]->frame_active) {
            display__unlock();
            return BRUCE_ERR_BUSY;
        }
    }
    for (int i = 0; i < DISPLAY__MAX_CONTEXTS; ++i) {
        if (!s_contexts[i].in_use) continue;
        s_contexts[i].tiled = false;
        display__set_visibility_locked(&s_contexts[i]);
    }
    for (size_t i = 0; i < count; ++i) {
        if (targets[i]->lock != NULL) xSemaphoreTake(targets[i]->lock, portMAX_DELAY);
        targets[i]->tiled = true;
        targets[i]->viewport = tiles[i].rect;
        targets[i]->hidden = false;
        targets[i]->viewport_generation++;
        if (targets[i]->lock != NULL) xSemaphoreGive(targets[i]->lock);
        if (s_framebuffer != NULL) {
            for (int row = 0; row < tiles[i].rect.height; ++row) {
                memset(
                    &s_framebuffer[(tiles[i].rect.y + row) * s_fb_width + tiles[i].rect.x],
                    0,
                    (size_t)tiles[i].rect.width * sizeof(*s_framebuffer)
                );
            }
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
            if (!display__reset_context_slot(&s_contexts[i])) continue;
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
        if (context->gui_requested && state == BRUCE_PROCESS_FOREGROUND &&
            context->state != BRUCE_PROCESS_FOREGROUND) {
            context->clear_on_next_frame = true;
        }
        context->state = state;
        if (state != BRUCE_PROCESS_FOREGROUND && context->frame_active && !context->tiled)
            context->frame_noop = true;
        if (!context->frame_active) display__set_visibility_locked(context);
    }
    display__unlock();
}

void display__process_removed(bruce_process_id_t process_id) {
    display__ensure_lock();
    display__lock();
    if (s_initialized) display__release_render_mode_for_process_locked(process_id);
    display__process_context_t *context = display__find_context_locked(process_id);
    if (context != NULL) {
        context->hidden = true;
        context->tiled = false;
        context->frame_active = false;
        context->active_overlay = NULL;
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
    if (!s_buffered_rendering) {
        display__unlock();
        return BRUCE_ERR_UNSUPPORTED;
    }
    *out_color = s_framebuffer[y * s_fb_width + x];
    display__unlock();
    return BRUCE_OK;
}

bruce_result_t display__test_overlay_state(
    bruce_display_overlay_id_t overlay, bruce_display_rect_t *out_rect, bool *out_visible,
    uint32_t *out_generation
) {
    return display_overlay__test_state(overlay, out_rect, out_visible, out_generation);
}

bruce_result_t display__test_overlay_pixel(
    bruce_display_overlay_id_t overlay, int16_t x, int16_t y, bruce_display_color_t *out_color
) {
    return display_overlay__test_pixel(overlay, x, y, out_color);
}
