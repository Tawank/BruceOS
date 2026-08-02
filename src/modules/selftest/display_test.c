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
         (display__test_read_pixel(1, 3, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE ||
          display__test_read_pixel(1, 1, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK))) {
        printf("[selftest] display/rendering: FAIL, glyph or cursor\n");
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
