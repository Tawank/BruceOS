#include "display_test.h"

#include <stdio.h>

#include "core/display/display.h"
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
    if (display__test_read_pixel(0, 0, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_GREEN) {
        printf("[selftest] display/compositor: FAIL, local pixel translation\n");
        return false;
    }
    if (display__test_read_pixel(10, 15, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_YELLOW ||
        display__test_read_pixel(5, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_YELLOW ||
        display__test_read_pixel(10, 5, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_YELLOW ||
        display__test_read_pixel(15, 10, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK) {
        printf("[selftest] display/compositor: FAIL, arc geometry\n");
        return false;
    }
    if (display__test_read_pixel(23, 20, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_CYAN ||
        display__test_read_pixel(26, 23, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK) {
        printf("[selftest] display/compositor: FAIL, rounded rectangle corners\n");
        return false;
    }
    if (display__present() != BRUCE_OK) {
        printf("[selftest] display/compositor: FAIL, present\n");
        return false;
    }
    if (display__draw_pixel(1, 1, BRUCE_COLOR_WHITE) != BRUCE_OK || display__flush() != BRUCE_OK) {
        printf("[selftest] display/compositor: FAIL, compatibility flush\n");
        return false;
    }
    printf("[selftest] display/compositor: OK\n");
    return true;
}

bool selftest__run_display_svg_path_case(void) {
    static const char *clock_path =
        "M12,20A8,8 0 0,0 20,12A8,8 0 0,0 12,4A8,8 0 0,0 4,12A8,8 0 0,0 12,20M12,2A10,10 0 0,1 22,12A10,10 0 0,1 12,22C6.47,22 2,17.5 2,12A10,10 0 0,1 12,2M12.5,7V12.25L17,14.92L16.25,16.15L11,13V7H12.5Z";

    if (display__begin_frame() != BRUCE_OK) {
        printf("[selftest] display/svg_path: FAIL, could not begin frame\n");
        return false;
    }
    if (display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        display__draw_svg_path(0, 0, 24, 24, clock_path, BRUCE_COLOR_WHITE) != BRUCE_OK) {
        printf("[selftest] display/svg_path: FAIL, draw returned error\n");
        display__present();
        return false;
    }

    bruce_display_color_t pixel = 0;
    if (display__test_read_pixel(12, 4, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE) {
        printf("[selftest] display/svg_path: FAIL, top of inner clock face not drawn\n");
        display__present();
        return false;
    }
    if (display__test_read_pixel(12, 2, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE) {
        printf("[selftest] display/svg_path: FAIL, top of outer clock face not drawn\n");
        display__present();
        return false;
    }
    if (display__test_read_pixel(12, 12, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK) {
        printf("[selftest] display/svg_path: FAIL, center of clock face overdrawn\n");
        display__present();
        return false;
    }
    if (display__test_read_pixel(0, 0, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK) {
        printf("[selftest] display/svg_path: FAIL, background overdrawn\n");
        display__present();
        return false;
    }
    if (display__draw_svg_path(0, 0, 24, 24, NULL, BRUCE_COLOR_WHITE) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] display/svg_path: FAIL, NULL path not rejected\n");
        display__present();
        return false;
    }
    if (display__present() != BRUCE_OK) {
        printf("[selftest] display/svg_path: FAIL, present\n");
        return false;
    }
    printf("[selftest] display/svg_path: OK\n");
    return true;
}
