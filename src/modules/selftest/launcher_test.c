#include "launcher_test.h"

#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/manifest.h"
#include "core_sdk/result.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

/* ------------------------------------------------------------------------ */
/* Apps discovery: drives the Apps dialog to launch a discovered app. The
 * launcher menu itself only hands this frontend off asynchronously. */
/* ------------------------------------------------------------------------ */

typedef enum {
    LAUNCHER_TEST_STATE_ROOT_OPEN_APPS,
    LAUNCHER_TEST_STATE_APPS_SELECT_APP,
    LAUNCHER_TEST_STATE_APPS_GO_BACK,
    LAUNCHER_TEST_STATE_ROOT_EXIT,
    LAUNCHER_TEST_STATE_DONE,
} launcher_test_state_t;

static launcher_test_state_t s_launcher_test_state;
static const char *s_launcher_test_target_label;
static bool s_launcher_test_found;

static size_t
selftest__launcher_find_choice(const bruce_dialog_choice_t *choices, size_t choice_count, const char *label) {
    for (size_t i = 0; i < choice_count; ++i) {
        if (strstr(choices[i].label, label) != NULL) { return i; }
    }
    return (size_t)-1;
}

static bruce_result_t selftest__launcher_choice_provider(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)message;

    if (s_launcher_test_state == LAUNCHER_TEST_STATE_DONE) { return BRUCE_ERR_CANCELLED; }

    bool is_root = (title != NULL && strcmp(title, "Main Menu") == 0);

    if (s_launcher_test_state == LAUNCHER_TEST_STATE_ROOT_OPEN_APPS && is_root) {
        size_t index = selftest__launcher_find_choice(choices, choice_count, "Apps");
        if (index == (size_t)-1) { return BRUCE_ERR_CANCELLED; }
        *out_selected = index;
        s_launcher_test_state = LAUNCHER_TEST_STATE_APPS_SELECT_APP;
        return BRUCE_OK;
    }

    if (s_launcher_test_state == LAUNCHER_TEST_STATE_APPS_SELECT_APP && !is_root) {
        size_t index = selftest__launcher_find_choice(choices, choice_count, s_launcher_test_target_label);
        if (index != (size_t)-1) {
            s_launcher_test_found = true;
            *out_selected = index;
            s_launcher_test_state = LAUNCHER_TEST_STATE_APPS_GO_BACK;
            return BRUCE_OK;
        }
        /* App not found in Apps submenu: go back and exit. */
        size_t back = selftest__launcher_find_choice(choices, choice_count, "Back");
        if (back != (size_t)-1) { *out_selected = back; }
        s_launcher_test_state = LAUNCHER_TEST_STATE_ROOT_EXIT;
        return BRUCE_OK;
    }

    if (s_launcher_test_state == LAUNCHER_TEST_STATE_APPS_SELECT_APP && is_root) {
        /* The terminal launcher returns to its choice loop after spawning Apps.
         * Yield until Apps has consumed the shared test provider. */
        for (int waited = 0; waited < 200 && !s_launcher_test_found; waited += 10) {
            (void)runtime__delay(10);
        }
        s_launcher_test_state = LAUNCHER_TEST_STATE_ROOT_EXIT;
        return BRUCE_ERR_CANCELLED;
    }

    if (s_launcher_test_state == LAUNCHER_TEST_STATE_APPS_GO_BACK && !is_root) {
        size_t back = selftest__launcher_find_choice(choices, choice_count, "Back");
        if (back == (size_t)-1) { return BRUCE_ERR_CANCELLED; }
        *out_selected = back;
        s_launcher_test_state = LAUNCHER_TEST_STATE_ROOT_EXIT;
        return BRUCE_OK;
    }

    if (s_launcher_test_state == LAUNCHER_TEST_STATE_ROOT_EXIT && is_root) {
        s_launcher_test_state = LAUNCHER_TEST_STATE_DONE;
        return BRUCE_ERR_CANCELLED;
    }

    /* Unexpected state: cancel. */
    return BRUCE_ERR_CANCELLED;
}

bool selftest__run_launcher_apps_discovery_case(void) {
    const char *path = "/apps/launcher_test_app.js";
    storage__remove(path);

    char source[160];
    int length = snprintf(
        source,
        sizeof(source),
        "/*{\"appName\":\"Launcher Test App\",\"coreAbiVersion\":%u,\"stackSize\":4096,\"permissions\":[]}*/\n",
        (unsigned)BRUCE_CORE_ABI_VERSION
    );
    if (length <= 0 || (size_t)length >= sizeof(source) ||
        !storage__write_file_atomic(path, source, (size_t)length)) {
        printf("[selftest] launcher/apps: could not create test script\n");
        return false;
    }

    s_launcher_test_state = LAUNCHER_TEST_STATE_APPS_SELECT_APP;
    s_launcher_test_target_label = "launcher_test_app";
    s_launcher_test_found = false;

    dialog__test_set_choice_provider(selftest__launcher_choice_provider);
    int result = app_runner__run("apps", "", BRUCE_LAUNCH_BACKGROUND);
    bruce_result_t wait_result =
        result > 0 ? process__wait((bruce_process_id_t)result, 2000) : BRUCE_ERR_INVALID_ARGUMENT;
    dialog__test_set_choice_provider(NULL);

    storage__remove(path);

    bool ok = s_launcher_test_found && result > 0;
    if (!ok) {
        printf(
            "[selftest] launcher/apps: found=%d result=%d wait=%d\n",
            s_launcher_test_found,
            result,
            wait_result
        );
        return false;
    }

    printf("[selftest] launcher/apps: OK\n");
    return true;
}
