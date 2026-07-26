#include "display_test.h"

#include <stdio.h>

#include "core/display/display.h"
#include "core_sdk/display.h"
#include "core_sdk/result.h"

bool selftest__run_display_compositor_case(void)
{
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
        display__draw_pixel(0, 0, BRUCE_COLOR_GREEN) != BRUCE_OK) {
        return false;
    }
    bruce_display_color_t pixel = 0;
    if (display__test_read_pixel(0, 0, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_GREEN) {
        printf("[selftest] display/compositor: FAIL, local pixel translation\n");
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
