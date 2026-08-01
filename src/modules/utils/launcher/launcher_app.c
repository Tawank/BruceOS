#include "launcher_app.h"

#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define LAUNCHER__FALLBACK_APP "bruce_launcher"
#define LAUNCHER__SUPERVISOR_INTERVAL_MS 200u

static bool launcher__has_arg(int argc, char **argv, const char *arg) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], arg) == 0) return true;
    }
    return false;
}

static bool launcher__has_foreground(void) {
    bruce_process_snapshot_t processes[8];
    size_t count = 0;
    if (process__list(processes, 8, &count) != BRUCE_OK) return true;
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].state == BRUCE_PROCESS_FOREGROUND) return true;
    }
    return false;
}

static int launcher__start(bool gui, bool background) {
    const char *configured = config__get_launcher_app();
    const char *target = (configured != NULL && configured[0] != '\0') ? configured : LAUNCHER__FALLBACK_APP;
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
    return result;
}

int launcher_app_main(int argc, char **argv) {
    bool gui = app_runner__args_have_gui(argc, argv);
    bool background = app_runner__args_have_background(argc, argv);
    if (!launcher__has_arg(argc, argv, "-s")) {
        int result = launcher__start(gui, background);
        return result < 0 ? result : 0;
    }

    bruce_process_id_t child = BRUCE_PROCESS_ID_INVALID;
    if (runtime__delay(LAUNCHER__SUPERVISOR_INTERVAL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    for (;;) {
        if (child != BRUCE_PROCESS_ID_INVALID) {
            bruce_result_t wait_result = process__wait(child, LAUNCHER__SUPERVISOR_INTERVAL_MS);
            if (wait_result == BRUCE_ERR_TIMEOUT) continue;
            child = BRUCE_PROCESS_ID_INVALID;
        }
        if (launcher__has_foreground()) {
            if (runtime__delay(LAUNCHER__SUPERVISOR_INTERVAL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
            continue;
        }
        int result = launcher__start(true, false);
        if (result >= 0) child = (bruce_process_id_t)result;
        else {
            stdio__printf("Launcher failed to start with code %d\n", result);
            if (runtime__delay(LAUNCHER__SUPERVISOR_INTERVAL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
        }
    }
}
