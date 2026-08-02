#include "display_internal.h"

#include "core_sdk/display.h"

static const uint8_t s_font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50},
    {0x00, 0x08, 0x07, 0x03, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x2A, 0x1C, 0x7F, 0x1C, 0x2A},
    {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46},
    {0x21, 0x41, 0x49, 0x4D, 0x33},
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x31},
    {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x46, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x00, 0x14, 0x00, 0x00},
    {0x00, 0x40, 0x34, 0x00, 0x00},
    {0x00, 0x08, 0x14, 0x22, 0x41},
    {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08},
    {0x02, 0x01, 0x59, 0x09, 0x06},
    {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    {0x7C, 0x12, 0x11, 0x12, 0x7C},
    {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x73},
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32},
    {0x03, 0x01, 0x7F, 0x01, 0x03},
    {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03},
    {0x61, 0x59, 0x49, 0x4D, 0x43},
    {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20},
    {0x00, 0x41, 0x41, 0x41, 0x7F},
    {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x03, 0x07, 0x08, 0x00},
    {0x20, 0x54, 0x54, 0x78, 0x40},
    {0x7F, 0x28, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x28},
    {0x38, 0x44, 0x44, 0x28, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x00, 0x08, 0x7E, 0x09, 0x02},
    {0x18, 0xA4, 0xA4, 0x9C, 0x78},
    {0x7F, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x40, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00},
    {0x7C, 0x04, 0x78, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38},
    {0xFC, 0x18, 0x24, 0x24, 0x18},
    {0x18, 0x24, 0x24, 0x18, 0xFC},
    {0x7C, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3F, 0x44, 0x24},
    {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x4C, 0x90, 0x90, 0x90, 0x7C},
    {0x44, 0x64, 0x54, 0x4C, 0x44},
    {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x77, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00},
    {0x02, 0x01, 0x02, 0x04, 0x02},
};

const uint8_t *display_internal__font_glyph(char c) {
    if (c < DISPLAY__FONT_FIRST || c > DISPLAY__FONT_LAST) { return NULL; }
    return s_font_5x7[(int)c - DISPLAY__FONT_FIRST];
}

static void display__draw_char(display__process_context_t *context, int16_t x, int16_t y, char c) {
    const uint8_t *glyph = display_internal__font_glyph(c);
    if (glyph == NULL) { return; }
    if (!context->text_bg_transparent) {
        display_internal__fill_rect(
            x,
            y,
            (DISPLAY__FONT_WIDTH + 1) * context->text_size,
            (DISPLAY__FONT_HEIGHT + 1) * context->text_size,
            context->text_bg_color
        );
    }
    for (int16_t row = 0; row <= DISPLAY__FONT_HEIGHT; ++row) {
        int16_t col = 0;
        while (col < DISPLAY__FONT_WIDTH) {
            while (col < DISPLAY__FONT_WIDTH && !(glyph[col] & (1 << row))) ++col;
            int16_t start = col;
            while (col < DISPLAY__FONT_WIDTH && (glyph[col] & (1 << row))) ++col;
            if (col > start) {
                display_internal__fill_rect(
                    x + start * context->text_size,
                    y + row * context->text_size,
                    (col - start) * context->text_size,
                    context->text_size,
                    context->text_color
                );
            }
        }
    }
}

static int32_t display__string_width(const display__process_context_t *context, const char *text) {
    int32_t width = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p >= DISPLAY__FONT_FIRST && *p <= DISPLAY__FONT_LAST) {
            width += (DISPLAY__FONT_WIDTH + 1) * context->text_size;
        }
    }
    return width;
}

static void
display__draw_single_line(display__process_context_t *context, const char *text, int32_t x, int16_t y) {
    context->cursor_x = (int16_t)x;
    context->cursor_y = y;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p >= DISPLAY__FONT_FIRST && *p <= DISPLAY__FONT_LAST) {
            display__draw_char(context, context->cursor_x, context->cursor_y, *p);
            context->cursor_x += (DISPLAY__FONT_WIDTH + 1) * context->text_size;
        }
    }
}

static bruce_result_t
display__draw_aligned_string(const char *text, int16_t x, int16_t y, uint8_t alignment) {
    if (text == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    int32_t draw_x = x;
    int32_t width = display__string_width(context, text);
    if (alignment == 1) draw_x -= width / 2;
    else if (alignment == 2) draw_x -= width;
    display__draw_single_line(context, text, draw_x, y);
    display_internal__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_text_color(bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_color = color;
    display_internal__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_text_bg_color(uint32_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_bg_transparent = color >= 0x10000;
    if (!context->text_bg_transparent) context->text_bg_color = (bruce_display_color_t)color;
    display_internal__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_text_size(uint8_t size) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_size = size < 1 ? 1 : (size > 8 ? 8 : size);
    display_internal__unlock();
    return BRUCE_OK;
}

bruce_result_t display__set_cursor(int16_t x, int16_t y) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->cursor_x = x;
    context->cursor_y = y;
    display_internal__unlock();
    return BRUCE_OK;
}

bruce_result_t display__get_cursor(int16_t *x, int16_t *y) {
    if (x == NULL || y == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    *x = context->cursor_x;
    *y = context->cursor_y;
    display_internal__unlock();
    return BRUCE_OK;
}

bruce_result_t display__print(const char *text) {
    if (text == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    for (const char *p = text; *p != '\0'; ++p) {
        char c = *p;
        if (c == '\n') {
            context->cursor_x = 0;
            context->cursor_y += (DISPLAY__FONT_HEIGHT + 1) * context->text_size;
        } else if (c == '\r') {
            context->cursor_x = 0;
        } else if (c >= DISPLAY__FONT_FIRST && c <= DISPLAY__FONT_LAST) {
            display__draw_char(context, context->cursor_x, context->cursor_y, c);
            context->cursor_x += (DISPLAY__FONT_WIDTH + 1) * context->text_size;
        }
    }
    display_internal__unlock();
    return BRUCE_OK;
}

bruce_result_t display__println(const char *text) {
    bruce_result_t result = display__print(text);
    return result == BRUCE_OK ? display__print("\n") : result;
}

bruce_result_t display__draw_string(const char *text, int16_t x, int16_t y) {
    return display__draw_aligned_string(text, x, y, 0);
}

bruce_result_t display__draw_centre_string(const char *text, int16_t x, int16_t y) {
    return display__draw_aligned_string(text, x, y, 1);
}

bruce_result_t display__draw_right_string(const char *text, int16_t x, int16_t y) {
    return display__draw_aligned_string(text, x, y, 2);
}
