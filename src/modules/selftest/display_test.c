#include "display_test.h"

#include <stdio.h>

#include "core/display/display.h"
#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/result.h"

bool selftest__run_display_compositor_case(void) {
    int width = display__width();
    int height = display__height();
    if (width <= 0 || height <= 0) {
        printf("[selftest] display/compositor: FAIL, foreground viewport is %dx%d\n", width, height);
        return false;
    }
    if (display__begin_frame() != BRUCE_OK || display__begin_frame() != BRUCE_ERR_INVALID_STATE) {
        printf("[selftest] display/compositor: FAIL, nested frame was accepted\n");
        return false;
    }
    if (display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        display__draw_pixel(0, 0, BRUCE_COLOR_GREEN) != BRUCE_OK ||
        display__draw_arc(10, 10, 5, 0, 180, BRUCE_COLOR_YELLOW) != BRUCE_OK ||
        display__draw_round_rect(20, 20, 20, 12, 3, BRUCE_COLOR_CYAN) != BRUCE_OK ||
        display__draw_arc(10, 10, -1, 0, 180, BRUCE_COLOR_YELLOW) != BRUCE_ERR_INVALID_ARGUMENT) {
        return false;
    }
    bruce_display_color_t pixel = 0;
    bool buffered = config__get_display_buffered_rendering();
    if (buffered && (display__test_read_pixel(0, 0, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_GREEN)) {
        printf("[selftest] display/compositor: FAIL, local pixel translation\n");
        return false;
    }
    if (buffered &&
        (display__test_read_pixel(10, 15, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_YELLOW ||
         display__test_read_pixel(5, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_YELLOW ||
         display__test_read_pixel(10, 5, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_YELLOW ||
         display__test_read_pixel(15, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK)) {
        printf("[selftest] display/compositor: FAIL, arc geometry\n");
        return false;
    }
    if (buffered &&
        (display__test_read_pixel(23, 20, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_CYAN ||
         display__test_read_pixel(26, 23, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK)) {
        printf("[selftest] display/compositor: FAIL, rounded rectangle corners\n");
        return false;
    }
    if (display__present() != BRUCE_OK) {
        printf("[selftest] display/compositor: FAIL, present\n");
        return false;
    }
    if (display__begin_frame() != BRUCE_OK || display__draw_pixel(1, 1, BRUCE_COLOR_WHITE) != BRUCE_OK ||
        display__present() != BRUCE_OK) {
        printf("[selftest] display/compositor: FAIL, compatibility flush\n");
        return false;
    }
    printf("[selftest] display/compositor: OK\n");
    return true;
}

bool selftest__run_display_rendering_case(void) {
    static const uint8_t bitmap[] = {0x80};
    static const uint8_t symmetric_bitmap[] = {0x90};

    if (display__begin_frame() != BRUCE_OK || display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        display__set_text_color(BRUCE_COLOR_WHITE) != BRUCE_OK ||
        display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT) != BRUCE_OK ||
        display__set_text_size(1) != BRUCE_OK || display__set_cursor(1, 1) != BRUCE_OK ||
        display__print("A") != BRUCE_OK) {
        printf("[selftest] display/rendering: FAIL, text setup or draw\n");
        return false;
    }

    int16_t cursor_x = 0;
    int16_t cursor_y = 0;
    bruce_display_color_t pixel = 0;
    bool buffered = config__get_display_buffered_rendering();
    if (display__get_cursor(&cursor_x, &cursor_y) != BRUCE_OK || cursor_x != 7 || cursor_y != 1 ||
        (buffered &&
         (display__test_read_pixel(1, 6, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(1, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK))) {
        printf("[selftest] display/rendering: FAIL, glyph or cursor\n");
        (void)display__present();
        return false;
    }

    if (display__set_cursor(70, 1) != BRUCE_OK || display__print("ąóý čťŕ “ èûığş") != BRUCE_OK ||
        display__get_cursor(&cursor_x, &cursor_y) != BRUCE_OK || cursor_x != 160 || cursor_y != 1 ||
        (buffered &&
         (display__test_read_pixel(72, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(73, 8, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(79, 3, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE))) {
        printf("[selftest] display/rendering: FAIL, UTF-8 glyphs or cursor\n");
        (void)display__present();
        return false;
    }

    /* È = E + grave: rows 0-1 of the cell are reserved for the accent, so it
     * must land clear of E's own ink (which starts at row 2) instead of
     * merging into/vanishing against the letter's top stroke the way the old
     * draw-time accent overlay did (see display_font_bitmap.c). */
    if (display__draw_string("ZÈ", 40, 10) != BRUCE_OK ||
        (buffered &&
         (display__test_read_pixel(41, 13, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(46, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(47, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(48, 11, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(46, 16, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(46, 19, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK))) {
        printf("[selftest] display/rendering: FAIL, capital glyph geometry\n");
        (void)display__present();
        return false;
    }

    if (display__draw_string("ťů", 60, 10) != BRUCE_OK ||
        (buffered &&
         (display__test_read_pixel(61, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(64, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(68, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK))) {
        printf("[selftest] display/rendering: FAIL, Czech accent geometry\n");
        (void)display__present();
        return false;
    }

    if (display__draw_string("Příliš žluťoučký ků", 1, 20) != BRUCE_OK ||
        display__get_cursor(&cursor_x, &cursor_y) != BRUCE_OK || cursor_x != 115 || cursor_y != 20) {
        printf("[selftest] display/rendering: FAIL, Czech UTF-8 text\n");
        (void)display__present();
        return false;
    }

    if (display__set_cursor(1, 30) != BRUCE_OK || display__print("Příliš žluťou") != BRUCE_OK ||
        (buffered &&
         (display__test_read_pixel(73, 34, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE)) ||
        display__print("č") != BRUCE_OK || display__get_cursor(&cursor_x, &cursor_y) != BRUCE_OK ||
        cursor_x != 85 || cursor_y != 30 ||
        (buffered &&
         (display__test_read_pixel(73, 34, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE))) {
        printf("[selftest] display/rendering: FAIL, Czech caron damaged preceding text\n");
        (void)display__present();
        return false;
    }

    if (display__draw_string("A", 10, 10) != BRUCE_OK ||
        display__get_cursor(&cursor_x, &cursor_y) != BRUCE_OK || cursor_x != 16 || cursor_y != 10 ||
        display__draw_centre_string("A", 20, 11) != BRUCE_OK ||
        display__get_cursor(&cursor_x, &cursor_y) != BRUCE_OK || cursor_x != 23 || cursor_y != 11 ||
        display__draw_right_string("A", 30, 12) != BRUCE_OK ||
        display__get_cursor(&cursor_x, &cursor_y) != BRUCE_OK || cursor_x != 30 || cursor_y != 12 ||
        display__draw_string(NULL, 0, 0) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] display/rendering: FAIL, aligned text\n");
        (void)display__present();
        return false;
    }

    if (display__set_text_bg_color(BRUCE_COLOR_RED) != BRUCE_OK ||
        display__draw_bitmap(20, 1, bitmap, 8, 1, BRUCE_COLOR_GREEN) != BRUCE_OK ||
        (buffered &&
         (display__test_read_pixel(20, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_GREEN ||
          display__test_read_pixel(21, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_RED))) {
        printf("[selftest] display/rendering: FAIL, bitmap colors\n");
        (void)display__present();
        return false;
    }

    /* Cyrillic/Greek/emoji glyph coverage (see display_font_bitmap.c). The
     * bitmap-colors check just above left text_bg_color = RED; restore
     * transparent so these checks can assume "not ink" reads back as the
     * BLACK screen fill, like every other check in this function does. */
    if (display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT) != BRUCE_OK ||
        display__set_cursor(1, 50) != BRUCE_OK || display__print("\xd0\x98") != BRUCE_OK ||
        /* U+0418 "И" is a mirrored "N" (see tools/gen_display_font.py's
         * mirror()) -- checking both that the diagonal is actually mirrored
         * (col 3 lit, not col 1) rather than accidentally reusing N's own
         * bitmap. */
        (buffered &&
         (display__test_read_pixel(2, 54, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(4, 54, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE))) {
        printf("[selftest] display/rendering: FAIL, Cyrillic glyph\n");
        (void)display__present();
        return false;
    }

    if (display__set_cursor(20, 50) != BRUCE_OK || display__print("\xce\xa9") != BRUCE_OK ||
        /* U+03A9 "Ω": arch top (row 2 lit) with an open middle (row 4 not
         * lit on the sides, row 8 not lit in the center) -- not just a
         * solid block. */
        (buffered &&
         (display__test_read_pixel(21, 52, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(21, 54, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(22, 58, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK))) {
        printf("[selftest] display/rendering: FAIL, Greek glyph\n");
        (void)display__present();
        return false;
    }

    /* U+1F680 (rocket) is above U+FFFF -- this is the case that would
     * silently regress to the hollow "tofu" fallback box if
     * display__extended_glyph_t.codepoint were ever narrowed back to
     * uint16_t (it would truncate the stored codepoint and the binary
     * search would miss). Tofu's interior is hollow, so a lit center pixel
     * only happens if the real glyph was found. */
    if (display__set_cursor(40, 50) != BRUCE_OK || display__print("\xf0\x9f\x9a\x80") != BRUCE_OK ||
        (buffered && (display__test_read_pixel(42, 54, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE))) {
        printf("[selftest] display/rendering: FAIL, astral-plane emoji glyph\n");
        (void)display__present();
        return false;
    }

    if (display__draw_bitmap_scaled(30, 1, bitmap, 8, 1, 16, 2, BRUCE_COLOR_CYAN) != BRUCE_OK ||
        display__draw_bitmap_scaled(50, 1, symmetric_bitmap, 4, 1, 6, 1, BRUCE_COLOR_CYAN) != BRUCE_OK ||
        (buffered &&
         (display__test_read_pixel(30, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_CYAN ||
          display__test_read_pixel(32, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(50, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_CYAN ||
          display__test_read_pixel(51, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(54, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK ||
          display__test_read_pixel(55, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_CYAN)) ||
        display__color565(255, 0, 0) != BRUCE_COLOR_RED || display__present() != BRUCE_OK) {
        printf("[selftest] display/rendering: FAIL, scaled bitmap, color conversion, or present\n");
        return false;
    }

    printf("[selftest] display/rendering: OK\n");
    return true;
}
