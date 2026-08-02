#include "launcher_test.h"

#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "core_sdk/process.h"
#include "fake_elf.h"
#include "modules/loaders/elf/elf_loader_app.h"

/* ------------------------------------------------------------------------ */
/* Launcher menu: drives the dialog__choice provider to navigate the JSON   */
/* menu, open the "Apps" submenu, launch the discovered app, then exit.    */
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
    const char *path = "/apps/launcher_test_app.elf";
    storage__remove(path);

    if (!selftest__write_fake_elf(path, "Launcher Test App", NULL, 0)) {
        printf("[selftest] launcher/apps: could not create fake ELF\n");
        return false;
    }

    s_launcher_test_state = LAUNCHER_TEST_STATE_ROOT_OPEN_APPS;
    s_launcher_test_target_label = "Launcher Test App";
    s_launcher_test_found = false;

    dialog__test_set_choice_provider(selftest__launcher_choice_provider);
    size_t calls_before = elf_loader__debug_call_count();
    int result = app_runner__run("bruce_launcher", "", BRUCE_LAUNCH_BACKGROUND);
    bruce_result_t wait_result =
        result > 0 ? process__wait((bruce_process_id_t)result, 5000) : BRUCE_ERR_INVALID_ARGUMENT;
    dialog__test_set_choice_provider(NULL);

    storage__remove(path);

    bool ok = s_launcher_test_found && result > 0 && wait_result == BRUCE_OK &&
              elf_loader__debug_call_count() == calls_before + 1;
    if (!ok) {
        printf(
            "[selftest] launcher/apps: found=%d result=%d wait=%d calls %zu -> %zu\n",
            s_launcher_test_found,
            result,
            wait_result,
            calls_before,
            elf_loader__debug_call_count()
        );
        return false;
    }

    printf("[selftest] launcher/apps: OK\n");
    return true;
}
