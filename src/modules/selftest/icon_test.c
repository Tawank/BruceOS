#include "icon_test.h"

#include <stdio.h>
#include <string.h>

#include "core/display/display.h"
#include "core_sdk/display.h"
#include "core_sdk/icon.h"
#include "core_sdk/result.h"

bool selftest__run_icon_registry_case(void) {
    static const char *const names[] = {
        "wifi", "bluetooth", "bt", "ir", "folder", "files", "terminal", "clock",
        "settings", "cog", "selftest", "test-tube", "apps",
    };
    for (size_t i = 0; i < (sizeof(names) / sizeof(names[0])); ++i) {
        const char *path = icon__get(names[i]);
        if (path == NULL || path[0] == '\0') {
            printf("[selftest] icon/registry: FAIL, missing icon '%s'\n", names[i]);
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

    /* Verify at least one icon renders successfully through display__fill_svg_path. */
    const char *clock = icon__get("clock");
    if (display__begin_frame() != BRUCE_OK) {
        printf("[selftest] icon/registry: FAIL, could not begin frame\n");
        return false;
    }
    if (display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        display__fill_svg_path(0, 0, 24, 24, clock, BRUCE_COLOR_WHITE) != BRUCE_OK) {
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
    if (display__present() != BRUCE_OK) {
        printf("[selftest] icon/registry: FAIL, present\n");
        return false;
    }
    printf("[selftest] icon/registry: OK\n");
    return true;
}
