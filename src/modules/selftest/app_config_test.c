#include "app_config_test.h"

#include <stdio.h>
#include <string.h>

#include "core/process/process.h"
#include "core_sdk/app_config.h"
#include "core_sdk/process.h"

/* app_config__(get|set)_*(app_name, ...) draws a hard line at app_name ==
 * NULL: NULL always resolves to the calling process's own identity (see
 * core_sdk/app_config.h and core/app_config/app_config.c), while an explicit
 * app_name naming some other app is restricted to built-in processes. These
 * two cases are exercised from a real spawned worker process (rather than
 * called straight from the built-in selftest task) so
 * process_registry__current_context() sees the identity/built_in flag this
 * is actually meant to gate on. */

static volatile bool s_self_worker_ok;
static volatile bool s_cross_app_worker_ok;

static int selftest__worker_app_config_self(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool ok = app_config__set_int(NULL, "selftest.value", 42) == BRUCE_OK;
    ok = ok && app_config__get_int(NULL, "selftest.value", -1) == 42;

    char out[BRUCE_APP_CONFIG_STRING_MAX_LEN];
    ok = ok && app_config__set_string(NULL, "selftest.text", "hello") == BRUCE_OK;
    ok = ok && app_config__get_string(NULL, "selftest.text", "", out, sizeof(out)) && strcmp(out, "hello") == 0;

    ok = ok && app_config__remove(NULL, "selftest.value") == BRUCE_OK;
    ok = ok && app_config__remove(NULL, "selftest.text") == BRUCE_OK;
    s_self_worker_ok = ok;
    return ok ? 0 : -1;
}

/* Non-built-in worker (a stand-in for a sandboxed ELF app) trying to reach
 * another app's config file by name outright -- every call must be refused
 * exactly like an invalid app_name (getters fall back to default, setters
 * return BRUCE_ERR_INVALID_ARGUMENT), and must not disturb that app's real
 * file. */
static int selftest__worker_app_config_cross_app(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool ok = app_config__set_int("some_other_app", "value", 1) != BRUCE_OK;
    ok = ok && app_config__get_int("some_other_app", "value", -99) == -99;
    ok = ok && app_config__remove("some_other_app", "value") != BRUCE_OK;
    s_cross_app_worker_ok = ok;
    return ok ? 0 : -1;
}

bool selftest__run_app_config_self_identity_case(void) {
    s_self_worker_ok = false;
    process_create_params_t params = {
        .name = "selftest_appcfg_self",
        .entry = selftest__worker_app_config_self,
        .built_in = false,
        .permission_key = "selftest_appcfg_self",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    bool ok = process_registry__create(&params, &id) == BRUCE_OK;
    bruce_process_status_t status;
    ok = ok && process__wait_status(id, 2000, &status) == BRUCE_OK && status.reason == BRUCE_PROCESS_EXITED &&
         status.exit_code == 0 && s_self_worker_ok;
    printf("[selftest] app_config/self-identity: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_app_config_cross_app_permission_case(void) {
    s_cross_app_worker_ok = false;
    process_create_params_t params = {
        .name = "selftest_appcfg_cross",
        .entry = selftest__worker_app_config_cross_app,
        .built_in = false,
        .permission_key = "selftest_appcfg_cross",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    bool ok = process_registry__create(&params, &id) == BRUCE_OK;
    bruce_process_status_t status;
    ok = ok && process__wait_status(id, 2000, &status) == BRUCE_OK && status.reason == BRUCE_PROCESS_EXITED &&
         status.exit_code == 0 && s_cross_app_worker_ok;
    printf("[selftest] app_config/cross-app-permission: %s\n", ok ? "OK" : "failed");
    return ok;
}
