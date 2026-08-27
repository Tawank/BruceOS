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

/* Resolves the configured launcher app (falling back to LAUNCHER__FALLBACK_APP)
 * and runs it with `arg` -- shared by plain "launcher" starts and by
 * "launcher config", which forwards to "<configured launcher> config" so the
 * settings screen always belongs to whichever app is actually configured to
 * run as the menu, not hardcoded to bruce_launcher. */
static int launcher__run_target(bool gui, bruce_launch_mode_t mode, const char *arg) {
    const char *configured = config__get_launcher();
    const char *target = (configured != NULL && configured[0] != '\0') ? configured : LAUNCHER__FALLBACK_APP;
    const bruce_environment_variable_t gui_env[] = {
        {.name = "GUI", .value = "1"}
    };

    int result = app_runner__run_with_environment(target, arg, mode, gui ? gui_env : NULL, gui ? 1u : 0u);
    if (result < 0 && strcmp(target, LAUNCHER__FALLBACK_APP) != 0) {
        stdio__printf(
            "launcherApp \"%s\" failed to start (%d); falling back to " LAUNCHER__FALLBACK_APP "\n",
            target,
            result
        );
        result = app_runner__run_with_environment(
            LAUNCHER__FALLBACK_APP, arg, mode, gui ? gui_env : NULL, gui ? 1u : 0u
        );
    }
    return result;
}

static int launcher__start(bool gui, bruce_launch_mode_t mode) {
    return launcher__run_target(gui, mode, NULL);
}

int launcher_app_main(int argc, char **argv) {
    if (launcher__has_arg(argc, argv, "--help") || launcher__has_arg(argc, argv, "-h")) {
        stdio__printf("Launches configured launcher.\nUsage: launcher [config|-s]\n");
        return BRUCE_OK;
    }
    if (argc > 1 && strcmp(argv[1], "config") == 0) {
        int result = launcher__run_target(runtime__gui_requested(), BRUCE_LAUNCH_FOREGROUND, "config");
        return result < 0 ? result : 0;
    }

    if (!launcher__has_arg(argc, argv, "-s")) {
        bruce_process_snapshot_t snapshot;
        bruce_launch_mode_t mode = BRUCE_LAUNCH_FOREGROUND;
        if (process__snapshot(process__current_id(), &snapshot) == BRUCE_OK &&
            snapshot.state == BRUCE_PROCESS_BACKGROUND) {
            mode = BRUCE_LAUNCH_BACKGROUND;
        }
        bool gui = runtime__gui_requested();
        int result = launcher__start(gui, mode);
        return result < 0 ? result : 0;
    }

    bruce_process_id_t child = BRUCE_PROCESS_ID_INVALID;
    if (runtime__delay(LAUNCHER__SUPERVISOR_INTERVAL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    for (;;) {
        if (child != BRUCE_PROCESS_ID_INVALID) {
            bruce_result_t wait_result = process__wait(child, LAUNCHER__SUPERVISOR_INTERVAL_MS);
            if (wait_result == BRUCE_ERR_TIMEOUT) continue;
            child = BRUCE_PROCESS_ID_INVALID;
            if (runtime__delay(LAUNCHER__SUPERVISOR_INTERVAL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
        }
        if (launcher__has_foreground()) {
            if (runtime__delay(LAUNCHER__SUPERVISOR_INTERVAL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
            continue;
        }
        int result = launcher__start(true, BRUCE_LAUNCH_FOREGROUND);
        if (result >= 0) child = (bruce_process_id_t)result;
        else {
            stdio__printf("Launcher failed to start with code %d\n", result);
            if (runtime__delay(LAUNCHER__SUPERVISOR_INTERVAL_MS) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
        }
    }
}
