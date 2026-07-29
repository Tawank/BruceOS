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
