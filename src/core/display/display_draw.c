#include "display_internal.h"

#include <math.h>
#include <stdlib.h>

#include "core_sdk/display.h"

static void display__draw_line_bresenham(
    display__process_context_t *context, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
    bruce_display_color_t color
) {
    if (y0 == y1) {
        if (x0 > x1) { int16_t swap = x0; x0 = x1; x1 = swap; }
        display_internal__fill_rect(context, x0, y0, x1 - x0 + 1, 1, color);
        return;
    }
    if (x0 == x1) {
        if (y0 > y1) { int16_t swap = y0; y0 = y1; y1 = swap; }
        display_internal__fill_rect(context, x0, y0, 1, y1 - y0 + 1, color);
        return;
    }
    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;
    for (;;) {
        display_internal__set_pixel(context, x0, y0, color);
        if (x0 == x1 && y0 == y1) { break; }
        int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void display__draw_circle_helper(
    display__process_context_t *context, int16_t cx, int16_t cy, int16_t r, bruce_display_color_t color,
    bool fill
) {
    int16_t x = 0;
    int16_t y = r;
    int16_t d = 3 - 2 * r;
    while (x <= y) {
        if (fill) {
            display__draw_line_bresenham(context, cx - x, cy + y, cx + x, cy + y, color);
            display__draw_line_bresenham(context, cx - x, cy - y, cx + x, cy - y, color);
            display__draw_line_bresenham(context, cx - y, cy + x, cx + y, cy + x, color);
            display__draw_line_bresenham(context, cx - y, cy - x, cx + y, cy - x, color);
        } else {
            display_internal__set_pixel(context, cx + x, cy + y, color);
            display_internal__set_pixel(context, cx - x, cy + y, color);
            display_internal__set_pixel(context, cx + x, cy - y, color);
            display_internal__set_pixel(context, cx - x, cy - y, color);
            display_internal__set_pixel(context, cx + y, cy + x, color);
            display_internal__set_pixel(context, cx - y, cy + x, color);
            display_internal__set_pixel(context, cx + y, cy - x, color);
            display_internal__set_pixel(context, cx - y, cy - x, color);
        }
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

enum {
    DISPLAY__CIRCLE_TOP_LEFT = 1 << 0,
    DISPLAY__CIRCLE_TOP_RIGHT = 1 << 1,
    DISPLAY__CIRCLE_BOTTOM_LEFT = 1 << 2,
    DISPLAY__CIRCLE_BOTTOM_RIGHT = 1 << 3,
};

static void display__draw_circle_quadrants(
    display__process_context_t *context, int16_t cx, int16_t cy, int16_t r, uint8_t quadrants,
    bruce_display_color_t color
) {
    int16_t x = 0;
    int16_t y = r;
    int16_t d = 3 - 2 * r;
    while (x <= y) {
        if (quadrants & DISPLAY__CIRCLE_TOP_LEFT) {
            display_internal__set_pixel(context, cx - x, cy - y, color);
            display_internal__set_pixel(context, cx - y, cy - x, color);
        }
        if (quadrants & DISPLAY__CIRCLE_TOP_RIGHT) {
            display_internal__set_pixel(context, cx + x, cy - y, color);
            display_internal__set_pixel(context, cx + y, cy - x, color);
        }
        if (quadrants & DISPLAY__CIRCLE_BOTTOM_LEFT) {
            display_internal__set_pixel(context, cx - x, cy + y, color);
            display_internal__set_pixel(context, cx - y, cy + x, color);
        }
        if (quadrants & DISPLAY__CIRCLE_BOTTOM_RIGHT) {
            display_internal__set_pixel(context, cx + x, cy + y, color);
            display_internal__set_pixel(context, cx + y, cy + x, color);
        }
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

static void display__draw_arc_helper(
    display__process_context_t *context, int16_t cx, int16_t cy, int16_t r, int start_angle, int end_angle,
    bruce_display_color_t color
) {
    int sweep = end_angle - start_angle;
    if (sweep == 0) { return; }
    if (sweep < 0) { sweep %= 360; sweep += 360; }
    else if (sweep > 360) sweep = 360;

    int normalized_start = start_angle % 360;
    if (normalized_start < 0) normalized_start += 360;
    int16_t previous_x = cx;
    int16_t previous_y = cy + r;
    bool have_previous = false;
    for (int offset = 0; offset <= sweep; ++offset) {
        float radians = (float)(normalized_start + offset) * ((float)M_PI / 180.0f);
        int16_t x = (int16_t)lroundf((float)cx - sinf(radians) * r);
        int16_t y = (int16_t)lroundf((float)cy + cosf(radians) * r);
        if (!have_previous) { display_internal__set_pixel(context, x, y, color); have_previous = true; }
        else if (x != previous_x || y != previous_y) {
            display__draw_line_bresenham(context, previous_x, previous_y, x, y, color);
        }
        previous_x = x;
        previous_y = y;
    }
}

static void display__draw_triangle_fill(
    display__process_context_t *context, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2,
    int16_t y2, bruce_display_color_t color
) {
    if (y0 > y1) { int16_t t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y1 > y2) { int16_t t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    if (y0 > y1) { int16_t t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    int16_t dy1 = y1 - y0;
    int16_t dx1 = x1 - x0;
    int16_t dy2 = y2 - y0;
    int16_t dx2 = x2 - x0;
    int16_t dy3 = y2 - y1;
    int16_t dx3 = x2 - x1;
    for (int16_t y = y0; y <= y2; ++y) {
        if (dy1 == 0 && dy2 == 0) { break; }
        int32_t xa = (y <= y1 || dy1 == 0)
                         ? (dy1 == 0 ? x0 : (int32_t)x0 + (int32_t)dx1 * (y - y0) / dy1)
                         : (dy3 == 0 ? x1 : (int32_t)x1 + (int32_t)dx3 * (y - y1) / dy3);
        int32_t xb = dy2 == 0 ? x0 : (int32_t)x0 + (int32_t)dx2 * (y - y0) / dy2;
        if (xa > xb) { int32_t t = xa; xa = xb; xb = t; }
        display__draw_line_bresenham(context, (int16_t)xa, y, (int16_t)xb, y, color);
    }
}

bruce_display_color_t display__color565(uint8_t r, uint8_t g, uint8_t b) {
    return (bruce_display_color_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bruce_result_t display__fill_screen(bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display_internal__fill_rect(context, 0, 0, context->viewport.width, context->viewport.height, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__clear(void) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) return result;
    display_internal__fill_rect(
        context, 0, 0, context->viewport.width, context->viewport.height, context->text_bg_color
    );
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_pixel(int16_t x, int16_t y, bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display_internal__set_pixel(context, x, y, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_line(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, bruce_display_color_t color
) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display__draw_line_bresenham(context, x0, y0, x1, y1, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display__draw_line_bresenham(context, x, y, x + w - 1, y, color);
    display__draw_line_bresenham(context, x + w - 1, y, x + w - 1, y + h - 1, color);
    display__draw_line_bresenham(context, x + w - 1, y + h - 1, x, y + h - 1, color);
    display__draw_line_bresenham(context, x, y + h - 1, x, y, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display_internal__fill_rect(context, x, y, w, h, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display__draw_circle_helper(context, x, y, r, color, false);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display__draw_circle_helper(context, x, y, r, color, true);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_arc(
    int16_t x, int16_t y, int16_t r, int16_t start_angle, int16_t end_angle, bruce_display_color_t color
) {
    if (r < 0) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display__draw_arc_helper(context, x, y, r, start_angle, end_angle, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display__draw_line_bresenham(context, x0, y0, x1, y1, color);
    display__draw_line_bresenham(context, x1, y1, x2, y2, color);
    display__draw_line_bresenham(context, x2, y2, x0, y0, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__fill_triangle(
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bruce_display_color_t color
) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display__draw_triangle_fill(context, x0, y0, x1, y1, x2, y2, color);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_round_rect(
    int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color
) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    int16_t max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r <= 0) {
        display__draw_line_bresenham(context, x, y, x + w - 1, y, color);
        display__draw_line_bresenham(context, x + w - 1, y, x + w - 1, y + h - 1, color);
        display__draw_line_bresenham(context, x + w - 1, y + h - 1, x, y + h - 1, color);
        display__draw_line_bresenham(context, x, y + h - 1, x, y, color);
    } else {
        display__draw_line_bresenham(context, x + r, y, x + w - r - 1, y, color);
        display__draw_line_bresenham(context, x + r, y + h - 1, x + w - r - 1, y + h - 1, color);
        display__draw_line_bresenham(context, x, y + r, x, y + h - r - 1, color);
        display__draw_line_bresenham(context, x + w - 1, y + r, x + w - 1, y + h - r - 1, color);
        display__draw_circle_quadrants(context, x + r, y + r, r, DISPLAY__CIRCLE_TOP_LEFT, color);
        display__draw_circle_quadrants(context, x + w - r - 1, y + r, r, DISPLAY__CIRCLE_TOP_RIGHT, color);
        display__draw_circle_quadrants(context, x + r, y + h - r - 1, r, DISPLAY__CIRCLE_BOTTOM_LEFT, color);
        display__draw_circle_quadrants(
            context, x + w - r - 1, y + h - r - 1, r, DISPLAY__CIRCLE_BOTTOM_RIGHT, color
        );
    }
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__fill_round_rect(
    int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color
) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    int16_t max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    display_internal__fill_rect(context, x + r, y, w - 2 * r, h, color);
    display_internal__fill_rect(context, x, y + r, w, h - 2 * r, color);
    display__draw_circle_helper(context, x + r, y + r, r, color, true);
    display__draw_circle_helper(context, x + w - r - 1, y + r, r, color, true);
    display__draw_circle_helper(context, x + r, y + h - r - 1, r, color, true);
    display__draw_circle_helper(context, x + w - r - 1, y + h - r - 1, r, color, true);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_bitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
) {
    if (bitmap == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    int16_t byte_width = (w + 7) / 8;
    for (int16_t row = 0; row < h; ++row) {
        if (!context->text_bg_transparent) {
            display_internal__fill_rect(context, x, y + row, w, 1, context->text_bg_color);
        }
        int16_t col = 0;
        while (col < w) {
            while (col < w && !(bitmap[row * byte_width + col / 8] & (0x80 >> (col & 7)))) ++col;
            int16_t start = col;
            while (col < w && (bitmap[row * byte_width + col / 8] & (0x80 >> (col & 7)))) ++col;
            if (col > start) display_internal__fill_rect(context, x + start, y + row, col - start, 1, color);
        }
    }
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_xbitmap(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, bruce_display_color_t color
) {
    if (bitmap == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    int16_t byte_width = (w + 7) / 8;
    for (int16_t row = 0; row < h; ++row) {
        int16_t col = 0;
        while (col < w) {
            while (col < w && !(bitmap[row * byte_width + col / 8] & (1 << (col & 7)))) ++col;
            int16_t start = col;
            while (col < w && (bitmap[row * byte_width + col / 8] & (1 << (col & 7)))) ++col;
            if (col > start) display_internal__fill_rect(context, x + start, y + row, col - start, 1, color);
        }
    }
    display_internal__unlock(context);
    return BRUCE_OK;
}

static int16_t display_internal__scaled_coordinate(int16_t position, int16_t source_size, int16_t dest_size) {
    bool mirrored = position >= (dest_size + 1) / 2;
    int16_t near_position = mirrored ? dest_size - 1 - position : position;
    int16_t source_position =
        (int16_t)(((int32_t)(2 * near_position + 1) * source_size) / (2 * dest_size));
    return mirrored ? source_size - 1 - source_position : source_position;
}

bruce_result_t display__draw_bitmap_scaled(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, int16_t dw, int16_t dh,
    bruce_display_color_t color
) {
    if (bitmap == NULL || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    int16_t byte_width = (w + 7) / 8;
    for (int16_t row = 0; row < dh; ++row) {
        int16_t src_y = display_internal__scaled_coordinate(row, h, dh);
        const uint8_t *src_row = bitmap + src_y * byte_width;
        int16_t col = 0;
        while (col < dw) {
            while (col < dw) {
                int16_t src_col = display_internal__scaled_coordinate(col, w, dw);
                if (src_row[src_col / 8] & (0x80 >> (src_col & 7))) break;
                ++col;
            }
            int16_t start = col;
            while (col < dw) {
                int16_t src_col = display_internal__scaled_coordinate(col, w, dw);
                if (!(src_row[src_col / 8] & (0x80 >> (src_col & 7)))) break;
                ++col;
            }
            if (col > start) display_internal__fill_rect(context, x + start, y + row, col - start, 1, color);
        }
    }
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    if (bitmap == NULL || w <= 0 || h <= 0) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    display_internal__draw_rgb_bitmap(context, x, y, bitmap, w, h);
    display_internal__unlock(context);
    return BRUCE_OK;
}
