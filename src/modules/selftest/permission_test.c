/* A4 acceptance coverage: coarse permission checks, /config/permissions.json
 * persistence, manifest preflight, and GUI/terminal dialog dispatch.
 *
 * Every test drives permission__check()/permission__preflight() through the
 * public core_sdk/permission.h and core_sdk/dialog.h APIs; the "external
 * app" processes these tests need (permission__check() only applies non-trivial
 * logic to non-built-in processes) are created directly via the Core-private
 * process_registry__create(), the same way task_test.c/app_runner_test.c do,
 * since ELF/JS loading (A6/A7) doesn't exist yet to launch a real one. */
#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core/permission/permission.h"
#include "core/process/process.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#include "permission_test.h"

/* ------------------------------------------------------------------------ */
/* Mock dialog__choice() provider                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile int call_count;
    volatile size_t next_selection;
    volatile bool trap; /* being invoked while set is itself a failure */
    volatile bool trap_violated;
} selftest__dialog_mock_t;

static selftest__dialog_mock_t s_mock;

static bruce_result_t selftest__dialog_mock_provider(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title;
    (void)message;
    (void)choices;
    (void)choice_count;
    s_mock.call_count++;
    if (s_mock.trap) s_mock.trap_violated = true;
    *out_selected = s_mock.next_selection;
    return BRUCE_OK;
}

static void selftest__dialog_mock_reset(size_t selection) {
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.next_selection = selection;
    dialog__test_set_choice_provider(selftest__dialog_mock_provider);
}

static void selftest__dialog_mock_clear(void) { dialog__test_set_choice_provider(NULL); }

/* ------------------------------------------------------------------------ */
/* Helper: run permission__check() from a process with a chosen built_in/key    */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile bruce_result_t result;
    volatile bool ran;
} selftest__permcheck_result_t;

static selftest__permcheck_result_t s_permcheck;

static int selftest__permission_check_entry(int argc, char **argv) {
    bruce_permission_t permission;
    if (argc < 1 || !permission__from_name(argv[0], &permission)) {
        s_permcheck.result = BRUCE_ERR_INVALID_ARGUMENT;
        s_permcheck.ran = true;
        return -1;
    }
    s_permcheck.result = permission__check(permission);
    s_permcheck.ran = true;
    return 0;
}

static bruce_result_t
selftest__permission_check_as(bool built_in, const char *permission_key, const char *permission_name) {
    memset(&s_permcheck, 0, sizeof(s_permcheck));
    char *argv[1] = {(char *)permission_name};
    process_create_params_t params = {
        .name = "selftest_permcheck",
        .entry = selftest__permission_check_entry,
        .argc = 1,
        .argv = argv,
        .built_in = built_in,
        .gui_requested = false,
        .permission_key = permission_key,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) { return BRUCE_ERR_INTERNAL; }
    bruce_result_t wait_result = process__wait(id, 2000);
    if (wait_result != BRUCE_OK && wait_result != BRUCE_ERR_NOT_FOUND) { return BRUCE_ERR_TIMEOUT; }
    return s_permcheck.ran ? s_permcheck.result : BRUCE_ERR_INTERNAL;
}

/* ------------------------------------------------------------------------ */
/* Cases                                                                     */
/* ------------------------------------------------------------------------ */

bool selftest__run_permission_allow_case(void) {
    permission__test_reset();
    selftest__dialog_mock_reset(0 /* Allow */);

    bruce_result_t result = selftest__permission_check_as(false, "selftest_allow.elf", "wifi");
    int calls = s_mock.call_count;

    bool allowed = false;
    bool saved_ok =
        permission__get_saved("selftest_allow.elf", BRUCE_PERMISSION_WIFI, &allowed) == BRUCE_OK && allowed;

    selftest__dialog_mock_clear();

    bool ok = result == BRUCE_OK && calls == 1 && saved_ok;
    printf(
        "[selftest] permission/allow: %s (result=%d calls=%d saved_ok=%d)\n",
        ok ? "OK" : "FAIL",
        result,
        calls,
        saved_ok
    );
    return ok;
}

bool selftest__run_permission_deny_no_reprompt_case(void) {
    permission__test_reset();
    selftest__dialog_mock_reset(1 /* Deny */);

    bruce_result_t first = selftest__permission_check_as(false, "selftest_deny.elf", "http");
    int first_calls = s_mock.call_count;

    /* A second check for the same key/permission must reuse the saved
     * denial: tripping the trap means the code prompted again. */
    s_mock.trap = true;
    bruce_result_t second = selftest__permission_check_as(false, "selftest_deny.elf", "http");
    bool trap_violated = s_mock.trap_violated;
    int second_calls = s_mock.call_count;

    selftest__dialog_mock_clear();

    bool ok = first == BRUCE_ERR_PERMISSION && first_calls == 1 && second == BRUCE_ERR_PERMISSION &&
              !trap_violated && second_calls == first_calls;
    printf(
        "[selftest] permission/deny-no-reprompt: %s (first=%d second=%d calls=%d/%d trap_violated=%d)\n",
        ok ? "OK" : "FAIL",
        first,
        second,
        first_calls,
        second_calls,
        trap_violated
    );
    return ok;
}

bool selftest__run_permission_shared_basename_case(void) {
    permission__test_reset();
    selftest__dialog_mock_reset(0 /* Allow */);

    bruce_result_t first_process = selftest__permission_check_as(false, "shared.elf", "bt");
    int calls_after_first = s_mock.call_count;

    /* A different process instance with the *same* permission key must share
     * the decision without re-prompting. */
    s_mock.trap = true;
    bruce_result_t second_process = selftest__permission_check_as(false, "shared.elf", "bt");
    bool trap_violated = s_mock.trap_violated;

    selftest__dialog_mock_clear();

    bool ok =
        first_process == BRUCE_OK && calls_after_first == 1 && second_process == BRUCE_OK && !trap_violated;
    printf(
        "[selftest] permission/shared-basename: %s (first=%d second=%d trap_violated=%d)\n",
        ok ? "OK" : "FAIL",
        first_process,
        second_process,
        trap_violated
    );
    return ok;
}

bool selftest__run_permission_builtin_grant_case(void) {
    permission__test_reset();
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.trap = true; /* a built-in must never prompt */
    dialog__test_set_choice_provider(selftest__dialog_mock_provider);

    bruce_result_t result = selftest__permission_check_as(true, "", "gpio");
    bool trap_violated = s_mock.trap_violated;

    selftest__dialog_mock_clear();

    bool ok = result == BRUCE_OK && !trap_violated;
    printf(
        "[selftest] permission/builtin-grant: %s (result=%d trap_violated=%d)\n",
        ok ? "OK" : "FAIL",
        result,
        trap_violated
    );
    return ok;
}

bool selftest__run_permission_preflight_case(void) {
    permission__test_reset();

    selftest__dialog_mock_reset(0 /* Allow */);
    const char *wifi_only[] = {"wifi"};
    bruce_result_t first = permission__preflight("preflight.elf", wifi_only, 1);

    selftest__dialog_mock_reset(1 /* Deny */);
    const char *bt_only[] = {"bt"};
    bruce_result_t second = permission__preflight("preflight.elf", bt_only, 1);

    /* Re-running preflight for "wifi" (already known) must not re-prompt. */
    s_mock.trap = true;
    bruce_result_t third = permission__preflight("preflight.elf", wifi_only, 1);
    bool trap_violated = s_mock.trap_violated;

    selftest__dialog_mock_clear();

    bool wifi_allowed = false;
    bool bt_allowed = true;
    bool got_wifi = permission__get_saved("preflight.elf", BRUCE_PERMISSION_WIFI, &wifi_allowed) == BRUCE_OK;
    bool got_bt = permission__get_saved("preflight.elf", BRUCE_PERMISSION_BT, &bt_allowed) == BRUCE_OK;

    bool ok = first == BRUCE_OK && second == BRUCE_OK && third == BRUCE_OK && !trap_violated && got_wifi &&
              wifi_allowed && got_bt && !bt_allowed;
    printf(
        "[selftest] permission/preflight: %s (first=%d second=%d third=%d trap=%d wifi=%d/%d bt=%d/%d)\n",
        ok ? "OK" : "FAIL",
        first,
        second,
        third,
        trap_violated,
        got_wifi,
        wifi_allowed,
        got_bt,
        bt_allowed
    );
    return ok;
}

typedef enum {
    SELFTEST_BOUNDARY_EXECUTE,
    SELFTEST_BOUNDARY_PROCESS,
} selftest__boundary_operation_t;

typedef struct {
    volatile bruce_result_t result;
    volatile bool ran;
    selftest__boundary_operation_t operation;
    bruce_process_id_t target;
} selftest__boundary_result_t;

static selftest__boundary_result_t s_boundary;

static int selftest__boundary_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (s_boundary.operation == SELFTEST_BOUNDARY_EXECUTE) {
        s_boundary.result =
            (bruce_result_t)app_runner__run("selftest_missing_command", "", BRUCE_LAUNCH_BACKGROUND);
    } else {
        s_boundary.result = process__pause(s_boundary.target);
    }
    s_boundary.ran = true;
    return 0;
}

static bruce_result_t selftest__run_boundary_as(
    const char *permission_key, selftest__boundary_operation_t operation, bruce_process_id_t target
) {
    memset(&s_boundary, 0, sizeof(s_boundary));
    s_boundary.operation = operation;
    s_boundary.target = target;
    process_create_params_t params = {
        .name = "selftest_boundary",
        .entry = selftest__boundary_entry,
        .built_in = false,
        .permission_key = permission_key,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) return BRUCE_ERR_INTERNAL;
    bruce_result_t wait_result = process__wait(id, 2000);
    if (wait_result != BRUCE_OK && wait_result != BRUCE_ERR_NOT_FOUND) return BRUCE_ERR_TIMEOUT;
    return s_boundary.ran ? s_boundary.result : BRUCE_ERR_INTERNAL;
}

static int selftest__boundary_target_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (;;) runtime__delay(1000);
    return 0;
}

bool selftest__run_permission_protected_boundaries_case(void) {
    permission__test_reset();
    selftest__dialog_mock_reset(0);
    s_mock.trap = true;

    (void)permission__set("execute-denied.elf", BRUCE_PERMISSION_EXECUTE, false);
    (void)permission__set("execute-allowed.elf", BRUCE_PERMISSION_EXECUTE, true);
    bruce_result_t execute_denied =
        selftest__run_boundary_as("execute-denied.elf", SELFTEST_BOUNDARY_EXECUTE, BRUCE_PROCESS_ID_INVALID);
    bruce_result_t execute_allowed =
        selftest__run_boundary_as("execute-allowed.elf", SELFTEST_BOUNDARY_EXECUTE, BRUCE_PROCESS_ID_INVALID);

    process_create_params_t target_params = {
        .name = "selftest_control_target",
        .entry = selftest__boundary_target_entry,
        .built_in = true,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t target = BRUCE_PROCESS_ID_INVALID;
    bool target_created = process_registry__create(&target_params, &target) == BRUCE_OK;

    (void)permission__set("process-denied.elf", BRUCE_PERMISSION_PROCESS, false);
    (void)permission__set("process-allowed.elf", BRUCE_PERMISSION_PROCESS, true);
    bruce_result_t task_denied =
        target_created ? selftest__run_boundary_as("process-denied.elf", SELFTEST_BOUNDARY_PROCESS, target)
                       : BRUCE_ERR_INTERNAL;
    bruce_result_t task_allowed =
        target_created ? selftest__run_boundary_as("process-allowed.elf", SELFTEST_BOUNDARY_PROCESS, target)
                       : BRUCE_ERR_INTERNAL;

    bruce_process_snapshot_t snapshot;
    bool paused = target_created && process__snapshot(target, &snapshot) == BRUCE_OK &&
                  snapshot.state == BRUCE_PROCESS_PAUSED;
    if (target_created) (void)process__kill(target);
    bool trap_violated = s_mock.trap_violated;
    selftest__dialog_mock_clear();

    bool ok = execute_denied == BRUCE_ERR_PERMISSION && execute_allowed == BRUCE_ERR_NOT_FOUND &&
              task_denied == BRUCE_ERR_PERMISSION && task_allowed == BRUCE_OK && paused && !trap_violated;
    printf(
        "[selftest] permission/protected-boundaries: %s (execute=%d/%d process=%d/%d paused=%d trap=%d)\n",
        ok ? "OK" : "FAIL",
        execute_denied,
        execute_allowed,
        task_denied,
        task_allowed,
        paused,
        trap_violated
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Dialog GUI/terminal renderer dispatch                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile bool ran;
    volatile bool observed_gui;
} selftest__dialog_dispatch_t;

static selftest__dialog_dispatch_t s_dispatch;

static int selftest__dialog_dispatch_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bruce_dialog_choice_t choices[2] = {
        {.label = "A", .value = "a"},
        {.label = "B", .value = "b"},
    };
    size_t selected = 0;
    dialog__choice("t", "m", choices, 2, &selected);
    s_dispatch.observed_gui = dialog__test_last_call_was_gui();
    s_dispatch.ran = true;
    return 0;
}

static bool selftest__run_dialog_dispatch_as(bool gui_requested, bool *out_observed_gui) {
    memset(&s_dispatch, 0, sizeof(s_dispatch));
    process_create_params_t params = {
        .name = "selftest_dialog_dispatch",
        .entry = selftest__dialog_dispatch_entry,
        .argc = 0,
        .argv = NULL,
        .built_in = false,
        .gui_requested = gui_requested,
        .permission_key = "",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) { return false; }
    bruce_result_t wait_result = process__wait(id, 2000);
    if ((wait_result != BRUCE_OK && wait_result != BRUCE_ERR_NOT_FOUND) || !s_dispatch.ran) { return false; }
    *out_observed_gui = s_dispatch.observed_gui;
    return true;
}

bool selftest__run_dialog_gui_terminal_dispatch_case(void) {
    selftest__dialog_mock_reset(0);

    bool observed_gui = false;
    bool gui_ran = selftest__run_dialog_dispatch_as(true, &observed_gui);
    bool gui_ok = gui_ran && observed_gui;

    bool observed_terminal = false;
    bool terminal_ran = selftest__run_dialog_dispatch_as(false, &observed_terminal);
    bool terminal_ok = terminal_ran && !observed_terminal;

    selftest__dialog_mock_clear();

    bool ok = gui_ok && terminal_ok;
    printf(
        "[selftest] dialog/gui-terminal-dispatch: %s (gui_ran=%d observed_gui=%d terminal_ran=%d "
        "observed_terminal=%d)\n",
        ok ? "OK" : "FAIL",
        gui_ran,
        observed_gui,
        terminal_ran,
        observed_terminal
    );
    return ok;
}
