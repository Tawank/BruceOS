#include "display_internal.h"

#include "core_sdk/display.h"

/* Font-agnostic text renderer: walks a UTF-8 string, asks the active
 * display__font_t (display_internal__active_font(), see display_font.h) for
 * each codepoint's glyph, and blits it. The only provider today is the
 * built-in bitmap font (display_font_bitmap.c) -- this file no longer knows
 * anything about accents, Latin-1 decomposition, or pixel-level glyph
 * shapes; that all lives in the font provider now. */

static const display__font_t *s_active_font;

void display_internal__set_font(const display__font_t *font) { s_active_font = font; }

const display__font_t *display_internal__active_font(void) {
    if (s_active_font == NULL) { s_active_font = display_font_bitmap__instance(); }
    return s_active_font;
}

static size_t display__utf8_decode(const char *text, uint32_t *out_codepoint) {
    const uint8_t *p = (const uint8_t *)text;
    uint32_t codepoint = p[0];
    size_t length = 1;
    if (p[0] >= 0xC2 && p[0] <= 0xDF && (p[1] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        length = 2;
    } else if (p[0] >= 0xE0 && p[0] <= 0xEF && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        length = 3;
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
               (p[3] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 7) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                    ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        length = 4;
    }
    *out_codepoint = codepoint;
    return length;
}

/* Blits a resolved glyph's own pixels (run-length per row, same shape as the
 * old fixed-font loop) -- does not touch the background box, callers draw
 * that first if needed. */
static void display__blit_glyph(
    display__process_context_t *context, int16_t x, int16_t y, const display__glyph_t *glyph
) {
    int16_t s = context->text_size;
    for (uint8_t row = 0; row < glyph->height; ++row) {
        uint8_t col = 0;
        while (col < glyph->width) {
            while (col < glyph->width && !(glyph->columns[col] & (1u << row))) ++col;
            uint8_t start = col;
            while (col < glyph->width && (glyph->columns[col] & (1u << row))) ++col;
            if (col > start) {
                display_internal__fill_rect(
                    context,
                    (int16_t)(x + start * s),
                    (int16_t)(y + row * s),
                    (int16_t)((col - start) * s),
                    s,
                    context->text_color
                );
            }
        }
    }
}

/* Hollow "tofu" box -- the conventional stand-in for a codepoint no active
 * font has a glyph for (comparable to .notdef in a real font, or the boxes
 * browsers/terminals show for unsupported characters), so a gap in Unicode
 * coverage reads as "no glyph" rather than silently vanishing or being
 * misrepresented as an unrelated character. */
static void display__draw_tofu(display__process_context_t *context, int16_t x, int16_t y) {
    const display__font_t *font = display_internal__active_font();
    int16_t s = context->text_size;
    int16_t w = (int16_t)(font->cell_width * s);
    int16_t h = (int16_t)(font->cell_height * s);
    display_internal__fill_rect(
        context, (int16_t)(x + s), (int16_t)(y + s), (int16_t)(w - 3 * s), s, context->text_color
    );
    display_internal__fill_rect(
        context, (int16_t)(x + s), (int16_t)(y + h - 2 * s), (int16_t)(w - 3 * s), s, context->text_color
    );
    display_internal__fill_rect(
        context, (int16_t)(x + s), (int16_t)(y + s), s, (int16_t)(h - 3 * s), context->text_color
    );
    display_internal__fill_rect(
        context, (int16_t)(x + w - 2 * s), (int16_t)(y + s), s, (int16_t)(h - 3 * s), context->text_color
    );
}

/* Draws one codepoint at (x, y) and returns its advance width in px (already
 * scaled by text_size), for the caller to move the cursor by. */
static int16_t
display__draw_char(display__process_context_t *context, int16_t x, int16_t y, uint32_t codepoint) {
    const display__font_t *font = display_internal__active_font();
    display__glyph_t glyph;
    bool found = font->get_glyph(codepoint, &glyph);
    int16_t s = context->text_size;
    int16_t advance = (int16_t)((found ? glyph.advance : font->cell_width) * s);

    if (!context->text_bg_transparent) {
        display_internal__fill_rect(
            context, x, y, advance, (int16_t)(font->cell_height * s), context->text_bg_color
        );
    }
    if (found) {
        display__blit_glyph(context, x, y, &glyph);
    } else {
        /* Callers only reach display__draw_char() for codepoint >= 0x20
         * (control characters are handled by display__print() itself). */
        display__draw_tofu(context, x, y);
    }
    return advance;
}

static int32_t display__string_width(const display__process_context_t *context, const char *text) {
    const display__font_t *font = display_internal__active_font();
    int32_t width = 0;
    for (const char *p = text; *p != '\0';) {
        uint32_t codepoint;
        size_t length = display__utf8_decode(p, &codepoint);
        if (codepoint >= 0x20) {
            display__glyph_t glyph;
            uint8_t advance = font->get_glyph(codepoint, &glyph) ? glyph.advance : font->cell_width;
            width += advance * context->text_size;
        }
        p += length;
    }
    return width;
}

static void
display__draw_single_line(display__process_context_t *context, const char *text, int32_t x, int16_t y) {
    context->cursor_x = (int16_t)x;
    context->cursor_y = y;
    for (const char *p = text; *p != '\0';) {
        uint32_t codepoint;
        size_t length = display__utf8_decode(p, &codepoint);
        if (codepoint >= 0x20) {
            context->cursor_x += display__draw_char(context, context->cursor_x, context->cursor_y, codepoint);
        }
        p += length;
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
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_text_color(bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_color = color;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_text_bg_color(uint32_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_bg_transparent = color >= 0x10000;
    if (!context->text_bg_transparent) context->text_bg_color = (bruce_display_color_t)color;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_text_size(uint8_t size) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_size = size < 1 ? 1 : (size > 8 ? 8 : size);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_cursor(int16_t x, int16_t y) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->cursor_x = x;
    context->cursor_y = y;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__get_cursor(int16_t *x, int16_t *y) {
    if (x == NULL || y == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    *x = context->cursor_x;
    *y = context->cursor_y;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__print(const char *text) {
    if (text == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    const display__font_t *font = display_internal__active_font();
    for (const char *p = text; *p != '\0';) {
        uint32_t codepoint;
        size_t length = display__utf8_decode(p, &codepoint);
        if (codepoint == '\n') {
            context->cursor_x = 0;
            context->cursor_y += (int16_t)(font->cell_height * context->text_size);
        } else if (codepoint == '\r') {
            context->cursor_x = 0;
        } else if (codepoint >= 0x20) {
            context->cursor_x += display__draw_char(context, context->cursor_x, context->cursor_y, codepoint);
        }
        p += length;
    }
    display_internal__unlock(context);
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

bruce_result_t display__get_font_metrics(int16_t *out_char_width, int16_t *out_char_height) {
    if (out_char_width == NULL || out_char_height == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    const display__font_t *font = display_internal__active_font();
    *out_char_width = (int16_t)font->cell_width;
    *out_char_height = (int16_t)font->cell_height;
    return BRUCE_OK;
}
