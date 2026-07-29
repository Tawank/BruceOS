#include "launcher_app.h"

#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/stdio.h"

#define LAUNCHER__FALLBACK_APP "bruce_launcher"

int launcher_app_main(int argc, char **argv) {
    const char *configured = config__get_launcher_app();
    const char *target = (configured != NULL && configured[0] != '\0') ? configured : LAUNCHER__FALLBACK_APP;
    bool gui = app_runner__args_have_gui(argc, argv);
    bool background = app_runner__args_have_background(argc, argv);
    const char *lifecycle_args = gui ? (background ? "--gui --bg" : "--gui") : (background ? "--bg" : "");

    int result = app_runner__run(target, lifecycle_args, true);
    if (result < 0 && strcmp(target, LAUNCHER__FALLBACK_APP) != 0) {
        stdio__printf(
            "launcherApp \"%s\" failed to start (%d); falling back to " LAUNCHER__FALLBACK_APP "\n",
            target,
            result
        );
        result = app_runner__run(LAUNCHER__FALLBACK_APP, lifecycle_args, true);
    }

    return result < 0 ? result : 0;
}
