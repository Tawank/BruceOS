#pragma once

/* Core-private display declarations. The public API is declared exactly once
 * in "core_sdk/display.h". Built-in modules and external apps must use only
 * core_sdk/display.h. */

#include "core_sdk/display.h"

void display__task_created(bruce_task_id_t task_id, bool gui_requested);
void display__task_set_gui_requested(bruce_task_id_t task_id);
void display__task_state_changed(bruce_task_id_t task_id, bruce_task_state_t state);
void display__task_removed(bruce_task_id_t task_id);

bruce_result_t display__notification_push(const char *text, uint32_t duration_ms);
bruce_result_t display__notification_dismiss(void);

/* Private on-device selftest seam. */
bruce_result_t display__test_read_pixel(int16_t x, int16_t y, bruce_display_color_t *out_color);
bruce_result_t display__test_notification(
    char *text, size_t text_size, bool *active, uint32_t *duration_ms, bruce_display_rect_t *rect,
    uint32_t *generation
);
