/* Generic always-on-top overlay pool: any process may create, draw into,
 * show/hide/move, and destroy a rectangular overlay (see core_sdk/display.h).
 * display.c calls back into this file to decide whether a transfer/direct
 * paint needs compositing and to actually do it; this file never touches
 * s_framebuffer, s_row_buffer, or the panel driver directly -- see
 * display_internal__stream_row_locked() / display_internal__repaint_rect_locked().
 *
 * Overlays are opaque rectangles: every pixel in an overlay's rect is drawn
 * over whatever is underneath, in pool-slot (creation) order, so there is no
 * transparency key to worry about matching bruce_display_color_t's 16-bit
 * range (BRUCE_COLOR_TRANSPARENT is a 32-bit sentinel used only by
 * display__set_text_bg_color() and does not fit a real pixel).
 *
 * All structural state (the s_overlays[] table itself, and each overlay's
 * rect/visible/id/generation) is guarded by the shared display registry
 * lock (display_internal__lock_registry()/unlock_registry(), the same one
 * display.c uses for its context table). Pixel data is guarded by the
 * overlay's own `surface.lock`, taken only via display_internal__begin_draw()
 * while a drawing session is open -- never together with another overlay's
 * or process's lock, and never across a registry-lock critical section. */

#include "display_internal.h"

#include <string.h>

#include "core/process/process.h"
#include "core_sdk/process.h"
#include "esp_heap_caps.h"

static display__overlay_t s_overlays[DISPLAY__MAX_OVERLAYS];
static uint32_t s_next_overlay_id;

static bool display_overlay__rects_overlap(bruce_display_rect_t a, bruce_display_rect_t b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

/* Caller must hold the registry lock. */
static display__overlay_t *display_overlay__find_locked(bruce_display_overlay_id_t id) {
    if (id == BRUCE_DISPLAY_OVERLAY_ID_INVALID) return NULL;
    for (int i = 0; i < DISPLAY__MAX_OVERLAYS; ++i) {
        if (s_overlays[i].in_use && s_overlays[i].id == id) return &s_overlays[i];
    }
    return NULL;
}

void display_overlay__deinit(void) {
    /* Overlays are owned by processes, not by the display subsystem's
     * init/deinit cycle -- process exit is what frees one, via the
     * process_registry__resource_register() cleanup installed in
     * display__overlay_create(), independent of whether the display
     * happens to be initialized at the time. A live overlay simply stops
     * compositing while deinitialized and resumes on the next
     * display__init(); there is nothing to tear down here. */
}

bool display_overlay__intersects_locked(bruce_display_rect_t rect) {
    for (int i = 0; i < DISPLAY__MAX_OVERLAYS; ++i) {
        if (s_overlays[i].in_use && s_overlays[i].visible &&
            display_overlay__rects_overlap(s_overlays[i].rect, rect)) {
            return true;
        }
    }
    return false;
}

void display_overlay__compose_row_locked(
    bruce_display_rect_t transfer, int screen_y, bruce_display_color_t *row_buffer
) {
    for (int i = 0; i < DISPLAY__MAX_OVERLAYS; ++i) {
        display__overlay_t *overlay = &s_overlays[i];
        if (!overlay->in_use || !overlay->visible) continue;
        bruce_display_rect_t rect = overlay->rect;
        if (screen_y < rect.y || screen_y >= rect.y + rect.height) continue;
        int left = rect.x > transfer.x ? rect.x : transfer.x;
        int right =
            (rect.x + rect.width) < (transfer.x + transfer.width) ? rect.x + rect.width : transfer.x + transfer.width;
        if (right <= left) continue;
        int local_y = screen_y - rect.y;
        const bruce_display_color_t *src =
            &overlay->pixels[(size_t)local_y * (size_t)rect.width + (size_t)(left - rect.x)];
        memcpy(&row_buffer[left - transfer.x], src, (size_t)(right - left) * sizeof(*row_buffer));
    }
}

bruce_result_t display_overlay__paint_direct_locked(bruce_display_rect_t clip) {
    for (int i = 0; i < DISPLAY__MAX_OVERLAYS; ++i) {
        display__overlay_t *overlay = &s_overlays[i];
        if (!overlay->in_use || !overlay->visible) continue;
        bruce_display_rect_t rect = overlay->rect;
        int left = rect.x > clip.x ? rect.x : clip.x;
        int right = (rect.x + rect.width) < (clip.x + clip.width) ? rect.x + rect.width : clip.x + clip.width;
        int top = rect.y > clip.y ? rect.y : clip.y;
        int bottom = (rect.y + rect.height) < (clip.y + clip.height) ? rect.y + rect.height : clip.y + clip.height;
        if (right <= left || bottom <= top) continue;
        for (int screen_y = top; screen_y < bottom; ++screen_y) {
            int local_y = screen_y - rect.y;
            const bruce_display_color_t *src =
                &overlay->pixels[(size_t)local_y * (size_t)rect.width + (size_t)(left - rect.x)];
            bruce_result_t result =
                display_internal__stream_row_locked((int16_t)left, (int16_t)screen_y, (int16_t)(right - left), src);
            if (result != BRUCE_OK) return result;
        }
    }
    return BRUCE_OK;
}

/* process_registry__* cleanup callback: runs when the owning process exits
 * or is killed without having called display__overlay_destroy() itself.
 * Must not block on anything but a brief lock and must not call
 * process_registry__* for a different process (see
 * bruce_process_resource_cleanup_t in core/process/process.h). */
static void display_overlay__cleanup(void *context) {
    display__overlay_t *overlay = (display__overlay_t *)context;
    display_internal__lock_registry();
    bruce_display_color_t *pixels = overlay->pixels;
    bruce_display_rect_t rect = overlay->rect;
    bool was_visible = overlay->visible;
    overlay->in_use = false;
    overlay->visible = false;
    overlay->pixels = NULL;
    overlay->surface.target_buffer = NULL;
    overlay->generation++;
    if (was_visible && display_internal__initialized()) (void)display_internal__repaint_rect_locked(rect);
    display_internal__unlock_registry();
    if (pixels != NULL) heap_caps_free(pixels);
}

bruce_result_t
display__overlay_create(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_overlay_id_t *out_overlay) {
    if (out_overlay == NULL || w <= 0 || h <= 0) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_id_t caller = process__current_id();
    display_internal__lock_registry();
    if (!display_internal__initialized()) {
        display_internal__unlock_registry();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    int16_t screen_w = 0;
    int16_t screen_h = 0;
    display_internal__screen_size(&screen_w, &screen_h);
    if (x < 0 || y < 0 || (int32_t)x + w > screen_w || (int32_t)y + h > screen_h) {
        display_internal__unlock_registry();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    int slot = -1;
    for (int i = 0; i < DISPLAY__MAX_OVERLAYS; ++i) {
        if (!s_overlays[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        display_internal__unlock_registry();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    display__overlay_t *overlay = &s_overlays[slot];
    if (overlay->surface.lock == NULL) {
        overlay->surface.lock = xSemaphoreCreateMutex();
        if (overlay->surface.lock == NULL) {
            display_internal__unlock_registry();
            return BRUCE_ERR_NO_MEMORY;
        }
    }
    overlay->in_use = true; /* reserve the slot before releasing the lock */
    overlay->owner = caller;
    if (++s_next_overlay_id == BRUCE_DISPLAY_OVERLAY_ID_INVALID) ++s_next_overlay_id;
    overlay->id = s_next_overlay_id;
    display_internal__unlock_registry();

    size_t pixel_count = (size_t)w * (size_t)h;
    bruce_display_color_t *pixels = heap_caps_malloc(pixel_count * sizeof(*pixels), MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        display_internal__lock_registry();
        overlay->in_use = false;
        display_internal__unlock_registry();
        return BRUCE_ERR_NO_MEMORY;
    }
    memset(pixels, 0, pixel_count * sizeof(*pixels));

    bruce_resource_id_t resource_id = BRUCE_RESOURCE_ID_INVALID;
    if (caller != BRUCE_PROCESS_ID_INVALID) {
        resource_id = process_registry__resource_register(display_overlay__cleanup, overlay);
        if (resource_id == BRUCE_RESOURCE_ID_INVALID) {
            heap_caps_free(pixels);
            display_internal__lock_registry();
            overlay->in_use = false;
            display_internal__unlock_registry();
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
    }

    display_internal__lock_registry();
    overlay->resource_id = resource_id;
    overlay->rect = (bruce_display_rect_t){x, y, w, h};
    overlay->pixels = pixels;
    overlay->visible = false;
    overlay->generation++;
    SemaphoreHandle_t surface_lock = overlay->surface.lock;
    memset(&overlay->surface, 0, sizeof(overlay->surface));
    overlay->surface.lock = surface_lock;
    overlay->surface.hidden = false;
    overlay->surface.viewport = (bruce_display_rect_t){0, 0, w, h};
    overlay->surface.target_buffer = pixels;
    overlay->surface.target_stride = w;
    overlay->surface.text_color = BRUCE_COLOR_WHITE;
    overlay->surface.text_bg_color = BRUCE_COLOR_BLACK;
    overlay->surface.text_size = 1;
    *out_overlay = overlay->id;
    display_internal__unlock_registry();
    return BRUCE_OK;
}

bruce_result_t display__overlay_destroy(bruce_display_overlay_id_t id) {
    bruce_process_id_t caller = process__current_id();
    display_internal__lock_registry();
    display__overlay_t *overlay = display_overlay__find_locked(id);
    if (overlay == NULL) {
        display_internal__unlock_registry();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (overlay->owner != caller) {
        display_internal__unlock_registry();
        return BRUCE_ERR_PERMISSION;
    }
    display__process_context_t *self = display_internal__find_context_locked(caller);
    if (self != NULL && self->active_overlay == overlay) {
        display_internal__unlock_registry();
        return BRUCE_ERR_BUSY;
    }
    bruce_resource_id_t resource_id = overlay->resource_id;
    bruce_display_color_t *pixels = overlay->pixels;
    bruce_display_rect_t rect = overlay->rect;
    bool was_visible = overlay->visible;
    overlay->in_use = false;
    overlay->visible = false;
    overlay->pixels = NULL;
    overlay->surface.target_buffer = NULL;
    overlay->generation++;
    bruce_result_t result = was_visible ? display_internal__repaint_rect_locked(rect) : BRUCE_OK;
    display_internal__unlock_registry();
    if (pixels != NULL) heap_caps_free(pixels);
    if (resource_id != BRUCE_RESOURCE_ID_INVALID) (void)process_registry__resource_release(resource_id);
    return result;
}

static bruce_result_t display_overlay__set_visible_locked_entry(bruce_display_overlay_id_t id, bool visible) {
    bruce_process_id_t caller = process__current_id();
    display_internal__lock_registry();
    display__overlay_t *overlay = display_overlay__find_locked(id);
    if (overlay == NULL) {
        display_internal__unlock_registry();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (overlay->owner != caller) {
        display_internal__unlock_registry();
        return BRUCE_ERR_PERMISSION;
    }
    if (overlay->visible == visible) {
        display_internal__unlock_registry();
        return BRUCE_OK;
    }
    overlay->visible = visible;
    overlay->generation++;
    bruce_result_t result = display_internal__repaint_rect_locked(overlay->rect);
    display_internal__unlock_registry();
    return result;
}

bruce_result_t display__overlay_show(bruce_display_overlay_id_t overlay) {
    return display_overlay__set_visible_locked_entry(overlay, true);
}

bruce_result_t display__overlay_hide(bruce_display_overlay_id_t overlay) {
    return display_overlay__set_visible_locked_entry(overlay, false);
}

bruce_result_t display__overlay_move(bruce_display_overlay_id_t id, int16_t x, int16_t y) {
    bruce_process_id_t caller = process__current_id();
    display_internal__lock_registry();
    display__overlay_t *overlay = display_overlay__find_locked(id);
    if (overlay == NULL) {
        display_internal__unlock_registry();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (overlay->owner != caller) {
        display_internal__unlock_registry();
        return BRUCE_ERR_PERMISSION;
    }
    int16_t screen_w = 0;
    int16_t screen_h = 0;
    display_internal__screen_size(&screen_w, &screen_h);
    if (x < 0 || y < 0 || (int32_t)x + overlay->rect.width > screen_w ||
        (int32_t)y + overlay->rect.height > screen_h) {
        display_internal__unlock_registry();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_display_rect_t old_rect = overlay->rect;
    overlay->rect.x = x;
    overlay->rect.y = y;
    overlay->generation++;
    bruce_result_t result = BRUCE_OK;
    if (overlay->visible) {
        result = display_internal__repaint_rect_locked(display_internal__rect_union(old_rect, overlay->rect));
    }
    display_internal__unlock_registry();
    return result;
}

bruce_result_t display__overlay_begin(bruce_display_overlay_id_t id) {
    bruce_process_id_t caller = process__current_id();
    display_internal__lock_registry();
    if (!display_internal__initialized()) {
        display_internal__unlock_registry();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    display__process_context_t *self = display_internal__find_context_locked(caller);
    if (self == NULL) {
        display_internal__unlock_registry();
        return BRUCE_ERR_PERMISSION;
    }
    if (self->active_overlay != NULL) {
        display_internal__unlock_registry();
        return BRUCE_ERR_INVALID_STATE;
    }
    display__overlay_t *overlay = display_overlay__find_locked(id);
    if (overlay == NULL) {
        display_internal__unlock_registry();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (overlay->owner != caller) {
        display_internal__unlock_registry();
        return BRUCE_ERR_PERMISSION;
    }
    self->active_overlay = overlay;
    display_internal__unlock_registry();
    return BRUCE_OK;
}

bruce_result_t display__overlay_end(bruce_display_overlay_id_t id) {
    bruce_process_id_t caller = process__current_id();
    display_internal__lock_registry();
    display__process_context_t *self = display_internal__find_context_locked(caller);
    display__overlay_t *overlay = display_overlay__find_locked(id);
    if (self == NULL || overlay == NULL || self->active_overlay != overlay) {
        display_internal__unlock_registry();
        return BRUCE_ERR_INVALID_STATE;
    }
    self->active_overlay = NULL;
    overlay->generation++;
    bruce_result_t result = overlay->visible ? display_internal__repaint_rect_locked(overlay->rect) : BRUCE_OK;
    display_internal__unlock_registry();
    return result;
}

bruce_result_t display_overlay__test_state(
    bruce_display_overlay_id_t id, bruce_display_rect_t *out_rect, bool *out_visible, uint32_t *out_generation
) {
    if (out_rect == NULL || out_visible == NULL || out_generation == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    display_internal__lock_registry();
    display__overlay_t *overlay = display_overlay__find_locked(id);
    if (overlay == NULL) {
        display_internal__unlock_registry();
        return BRUCE_ERR_NOT_FOUND;
    }
    *out_rect = overlay->rect;
    *out_visible = overlay->visible;
    *out_generation = overlay->generation;
    display_internal__unlock_registry();
    return BRUCE_OK;
}

bruce_result_t display_overlay__test_pixel(
    bruce_display_overlay_id_t id, int16_t x, int16_t y, bruce_display_color_t *out_color
) {
    if (out_color == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    display_internal__lock_registry();
    display__overlay_t *overlay = display_overlay__find_locked(id);
    if (overlay == NULL || overlay->pixels == NULL || x < 0 || y < 0 || x >= overlay->rect.width ||
        y >= overlay->rect.height) {
        display_internal__unlock_registry();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_color = overlay->pixels[(size_t)y * (size_t)overlay->rect.width + (size_t)x];
    display_internal__unlock_registry();
    return BRUCE_OK;
}
