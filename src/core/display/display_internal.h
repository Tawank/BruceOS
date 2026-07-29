#pragma once

#include "core_sdk/display.h"

#include "freertos/FreeRTOS.h"

#define DISPLAY__NATIVE_WIDTH 135
#define DISPLAY__NATIVE_HEIGHT 240
#define DISPLAY__FB_SIZE (DISPLAY__NATIVE_WIDTH * DISPLAY__NATIVE_HEIGHT * sizeof(bruce_display_color_t))

#define DISPLAY__FONT_WIDTH 5
#define DISPLAY__FONT_HEIGHT 7
#define DISPLAY__FONT_FIRST 32
#define DISPLAY__FONT_LAST 126

typedef struct {
    bool in_use;
    bool gui_requested;
    bool built_in;
    bool tiled;
    bool hidden;
    bool clear_on_next_frame;
    bool frame_active;
    bool frame_noop;
    bool transfer_pending;
    bool remove_pending;
    bruce_task_id_t task_id;
    bruce_task_state_t state;
    bruce_display_rect_t viewport;
    uint32_t viewport_generation;
    uint32_t frame_generation;
    bruce_display_color_t text_color;
    bruce_display_color_t text_bg_color;
    bool text_bg_transparent;
    uint8_t text_size;
    int16_t cursor_x;
    int16_t cursor_y;
    SemaphoreHandle_t completion;
    bruce_result_t completion_result;
} display__task_context_t;

bruce_result_t display_internal__begin_draw(display__task_context_t **context);
void display_internal__unlock(void);
void display_internal__set_pixel(int16_t x, int16_t y, bruce_display_color_t color);
void display_internal__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color);
const uint8_t *display_internal__font_glyph(char c);
bool display_internal__on_transfer_done_from_isr(void);
