#include "icon_test.h"

#include <stdio.h>
#include <string.h>

#include "core/display/display.h"
#include "core_sdk/display.h"
#include "core_sdk/icon.h"
#include "core_sdk/result.h"

bool selftest__run_icon_registry_case(void) {
    static const char *const names[] = {
        "apps",           "bluetooth", "clock-outline", "cog",     "console",
        "expansion-card", "folder",    "infrared",      "radio-handheld",
        "remote-tv",      "rfid",      "test-tube",     "wifi",
    };
    for (size_t i = 0; i < (sizeof(names) / sizeof(names[0])); ++i) {
        const bruce_icon_t *icon = icon__get(names[i]);
        if (icon == NULL || icon->bits == NULL || icon->width != BRUCE_ICON_SIZE ||
            icon->height != BRUCE_ICON_SIZE) {
            printf("[selftest] icon/registry: FAIL, missing icon '%s'\n", names[i]);
            return false;
        }
        /* Every built-in icon must have at least one set bit. */
        bool any_set = false;
        for (size_t byte = 0; byte < 72 && !any_set; ++byte) { any_set = icon->bits[byte] != 0; }
        if (!any_set) {
            printf("[selftest] icon/registry: FAIL, icon '%s' is empty\n", names[i]);
            return false;
        }
    }
    if (icon__get("no-such-icon") != NULL) {
        printf("[selftest] icon/registry: FAIL, unknown name returned non-NULL\n");
        return false;
    }
    if (icon__get(NULL) != NULL) {
        printf("[selftest] icon/registry: FAIL, NULL name returned non-NULL\n");
        return false;
    }

    /* Verify an icon renders through the scaled 1bpp bitmap path. */
    const bruce_icon_t *clock = icon__get("clock-outline");
    if (display__begin_frame() != BRUCE_OK) {
        printf("[selftest] icon/registry: FAIL, could not begin frame\n");
        return false;
    }
    if (display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        display__draw_bitmap_scaled(0, 0, clock->bits, clock->width, clock->height, 24, 24, BRUCE_COLOR_WHITE) !=
            BRUCE_OK) {
        printf("[selftest] icon/registry: FAIL, could not draw icon\n");
        display__present();
        return false;
    }
    bruce_display_color_t pixel = 0;
    if (display__test_read_pixel(12, 2, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE) {
        printf("[selftest] icon/registry: FAIL, icon did not draw\n");
        display__present();
        return false;
    }
    /* Clear bits must stay transparent (background untouched). */
    if (display__test_read_pixel(0, 0, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK) {
        printf("[selftest] icon/registry: FAIL, icon background not transparent\n");
        display__present();
        return false;
    }
    if (display__present() != BRUCE_OK) {
        printf("[selftest] icon/registry: FAIL, present\n");
        return false;
    }
    printf("[selftest] icon/registry: OK\n");
    return true;
}

bool selftest__run_display_svg_fill_case(void) {
    /* Clock-face ring: inner and outer circles wind in opposite directions
     * (nonzero rule), so the ring fills and the center stays hollow; the
     * clock hands are a third subpath filled inside the hollow center. */
    static const char *ring_path =
        "M12,20A8,8 0 0,0 20,12A8,8 0 0,0 12,4A8,8 0 0,0 4,12A8,8 0 0,0 12,20"
        "M12,2A10,10 0 0,1 22,12A10,10 0 0,1 12,22C6.47,22 2,17.5 2,12A10,10 0 0,1 12,2"
        "M12.5,7V12.25L17,14.92L16.25,16.15L11,13V7H12.5Z";

    if (display__begin_frame() != BRUCE_OK) {
        printf("[selftest] display/svg_fill: FAIL, could not begin frame\n");
        return false;
    }
    if (display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        display__fill_svg_path(0, 0, 24, 24, ring_path, BRUCE_COLOR_WHITE) != BRUCE_OK) {
        printf("[selftest] display/svg_fill: FAIL, fill returned error\n");
        display__present();
        return false;
    }

    bruce_display_color_t pixel = 0;
    if (display__test_read_pixel(12, 3, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE) {
        printf("[selftest] display/svg_fill: FAIL, ring band not filled\n");
        display__present();
        return false;
    }
    if (display__test_read_pixel(8, 12, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK) {
        printf("[selftest] display/svg_fill: FAIL, hollow center filled\n");
        display__present();
        return false;
    }
    if (display__test_read_pixel(12, 8, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_WHITE) {
        printf("[selftest] display/svg_fill: FAIL, hand subpath not filled\n");
        display__present();
        return false;
    }
    if (display__test_read_pixel(0, 0, &pixel) != BRUCE_OK || pixel != BRUCE_COLOR_BLACK) {
        printf("[selftest] display/svg_fill: FAIL, background overdrawn\n");
        display__present();
        return false;
    }
    if (display__fill_svg_path(0, 0, 24, 24, NULL, BRUCE_COLOR_WHITE) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] display/svg_fill: FAIL, NULL path not rejected\n");
        display__present();
        return false;
    }
    if (display__present() != BRUCE_OK) {
        printf("[selftest] display/svg_fill: FAIL, present\n");
        return false;
    }
    printf("[selftest] display/svg_fill: OK\n");
    return true;
}
