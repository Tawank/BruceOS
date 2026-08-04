#pragma once

#include "core_sdk/display.h"

#define DISPLAY__NATIVE_WIDTH CONFIG_BRUCE_DISPLAY_WIDTH
#define DISPLAY__NATIVE_HEIGHT CONFIG_BRUCE_DISPLAY_HEIGHT
#define DISPLAY__FB_SIZE (DISPLAY__NATIVE_WIDTH * DISPLAY__NATIVE_HEIGHT * sizeof(bruce_display_color_t))
#define DISPLAY__DIRECT_BUF_PIXELS                                                                        \
    (DISPLAY__NATIVE_HEIGHT * ((DISPLAY__NATIVE_WIDTH + 3) / 4))

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
    bruce_process_id_t process_id;
    bruce_process_state_t state;
    bruce_display_rect_t viewport;
    uint32_t viewport_generation;
    uint32_t frame_generation;
    bruce_result_t draw_result;
    bruce_display_color_t text_color;
    bruce_display_color_t text_bg_color;
    bool text_bg_transparent;
    uint8_t text_size;
    int16_t cursor_x;
    int16_t cursor_y;
} display__process_context_t;

bruce_result_t display_internal__begin_draw(display__process_context_t **context);
void display_internal__unlock(void);
void display_internal__set_pixel(int16_t x, int16_t y, bruce_display_color_t color);
void display_internal__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color);
void display_internal__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);
const uint8_t *display_internal__font_glyph(char c);
bool display_internal__on_transfer_done_from_isr(void);
