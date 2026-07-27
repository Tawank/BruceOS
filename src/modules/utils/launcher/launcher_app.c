#include "launcher_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"

#define LAUNCHER__FALLBACK_APP "bruce_launcher"

int launcher_app_main(int argc, char **argv) {
    char *configured = config__get_launcher_app();
    const char *target = (configured != NULL && configured[0] != '\0') ? configured : LAUNCHER__FALLBACK_APP;
    const char *gui_arg = app_runner__args_have_gui(argc, argv) ? "--gui" : "";

    int result = app_runner__run(target, gui_arg, false);
    if (result < 0 && strcmp(target, LAUNCHER__FALLBACK_APP) != 0) {
        printf(
            "launcherApp \"%s\" failed to start (%d); falling back to " LAUNCHER__FALLBACK_APP "\n",
            target,
            result
        );
        result = app_runner__run(LAUNCHER__FALLBACK_APP, gui_arg, false);
    }

    free(configured);
    return result < 0 ? result : 0;
}
