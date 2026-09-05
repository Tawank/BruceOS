#pragma once

/* Core-private display declarations. The public API is declared exactly once
 * in "core_sdk/display.h". Built-in modules and external apps must use only
 * core_sdk/display.h. */

#include "core_sdk/display.h"

bruce_result_t display__init(void);
/* Keep a GPIO-controlled backlight dark during the rest of system startup. */
void display__boot_backlight_off(void);
void display__deinit(void);
bruce_result_t display__flush(void);

void display__process_created(bruce_process_id_t process_id, bool gui_requested);
void display__process_set_gui_requested(bruce_process_id_t process_id);
void display__process_state_changed(bruce_process_id_t process_id, bruce_process_state_t state);
void display__process_removed(bruce_process_id_t process_id);

/* Private on-device selftest seam. */
bruce_result_t display__test_read_pixel(int16_t x, int16_t y, bruce_display_color_t *out_color);
/* Overlay introspection: rect/visible/generation for `overlay`, regardless
 * of who owns it (selftest is not the owner, so it can't use the public
 * display__overlay_* accessors, which are owner-only). */
bruce_result_t display__test_overlay_state(
    bruce_display_overlay_id_t overlay, bruce_display_rect_t *out_rect, bool *out_visible,
    uint32_t *out_generation
);
bruce_result_t display__test_overlay_pixel(
    bruce_display_overlay_id_t overlay, int16_t x, int16_t y, bruce_display_color_t *out_color
);
