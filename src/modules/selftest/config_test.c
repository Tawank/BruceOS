/* A5 acceptance coverage: field-specific Config getters/setters, `config`
 * permission enforcement, and the permanently-protected field group
 * (wifiApSsid/wifiApPassword, wifiCredentials, wifiMAC, webUIUser,
 * webUIPassword) that stays denied to an external task no matter what it
 * has been granted.
 *
 * Like permission_test.c/storage_test.c, the "external app" tasks these
 * tests need are created directly via the Core-private
 * task_registry__create() (no ELF/JS loader exists yet to launch a real
 * one). */
#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core/permission/permission.h"
#include "core/task/task.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/task.h"

#include "config_test.h"

/* ------------------------------------------------------------------------ */
/* Mock dialog__choice() provider (same pattern as permission_test.c)        */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile int call_count;
    volatile size_t next_selection;
} selftest__config_dialog_mock_t;

static selftest__config_dialog_mock_t s_mock;

static bruce_result_t selftest__config_dialog_mock_provider(const char *title, const char *message,
                                                             const bruce_dialog_choice_t *choices, size_t choice_count,
                                                             size_t *out_selected)
{
    (void)title;
    (void)message;
    (void)choices;
    (void)choice_count;
    s_mock.call_count++;
    *out_selected = s_mock.next_selection;
    return BRUCE_OK;
}

static void selftest__config_dialog_mock_reset(size_t selection)
{
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.next_selection = selection;
    dialog__test_set_choice_provider(selftest__config_dialog_mock_provider);
}

static void selftest__config_dialog_mock_clear(void)
{
    dialog__test_set_choice_provider(NULL);
}

/* ------------------------------------------------------------------------ */
/* Helper: run a body from a chosen built_in/permission_key task context     */
/* ------------------------------------------------------------------------ */

typedef int (*selftest__config_entry_t)(int argc, char **argv);

static bool selftest__config_run_as(bool built_in, const char *permission_key, selftest__config_entry_t entry)
{
    task_create_params_t params = {
        .name = "selftest_config",
        .entry = entry,
        .argc = 0,
        .argv = NULL,
        .built_in = built_in,
        .gui_requested = false,
        .permission_key = permission_key,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_task_id_t id = BRUCE_TASK_ID_INVALID;
    if (task_registry__create(&params, &id) != BRUCE_OK) return false;
    bruce_result_t wait_result = task__wait(id, 2000);
    return wait_result == BRUCE_OK || wait_result == BRUCE_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_permission_denied_case                              */
/* ------------------------------------------------------------------------ */

static volatile bruce_result_t s_config_get_result;
static volatile bruce_result_t s_config_set_result;

static int selftest__config_bright_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    s_config_set_result = config__set_bright(55);
    int value = -1;
    s_config_get_result = config__get_bright(&value);
    return 0;
}

bool selftest__run_config_permission_denied_case(void)
{
    permission__test_reset();
    selftest__config_dialog_mock_reset(1 /* Deny */);
    s_config_get_result = BRUCE_OK;
    s_config_set_result = BRUCE_OK;

    bool ran = selftest__config_run_as(false, "selftest_config_denied.elf", selftest__config_bright_entry);

    selftest__config_dialog_mock_clear();

    bool ok = ran && s_config_set_result == BRUCE_ERR_PERMISSION && s_config_get_result == BRUCE_ERR_PERMISSION;
    printf("[selftest] config/permission-denied: %s (set=%d get=%d)\n", ok ? "OK" : "FAIL", s_config_set_result,
           s_config_get_result);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_permission_allowed_case                             */
/* ------------------------------------------------------------------------ */

static volatile int s_config_bright_value;

static int selftest__config_bright_allowed_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    s_config_set_result = config__set_bright(66);
    int value = -1;
    s_config_get_result = config__get_bright(&value);
    s_config_bright_value = value;
    return 0;
}

bool selftest__run_config_permission_allowed_case(void)
{
    permission__test_reset();
    selftest__config_dialog_mock_reset(0 /* Allow */);
    s_config_get_result = BRUCE_ERR_INTERNAL;
    s_config_set_result = BRUCE_ERR_INTERNAL;
    s_config_bright_value = -1;

    bool ran = selftest__config_run_as(false, "selftest_config_allowed.elf", selftest__config_bright_allowed_entry);

    selftest__config_dialog_mock_clear();

    bool ok =
        ran && s_config_set_result == BRUCE_OK && s_config_get_result == BRUCE_OK && s_config_bright_value == 66;
    printf("[selftest] config/permission-allowed: %s (set=%d get=%d value=%d)\n", ok ? "OK" : "FAIL",
           s_config_set_result, s_config_get_result, s_config_bright_value);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_protected_field_denied_case                         */
/* ------------------------------------------------------------------------ */

static int selftest__config_protected_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char ssid[33] = {0};
    char password[65] = {0};
    s_config_get_result = config__get_wifi_ap(ssid, sizeof(ssid), password, sizeof(password));
    s_config_set_result = config__set_web_ui_password("not-allowed");
    return 0;
}

bool selftest__run_config_protected_field_denied_case(void)
{
    permission__test_reset();
    /* Mock answers Allow: even if the external task were to request and be
     * granted `config`, the permanently-protected group must still deny it -
     * these two calls never even consult permission__check()/the dialog. */
    selftest__config_dialog_mock_reset(0 /* Allow */);
    s_config_get_result = BRUCE_OK;
    s_config_set_result = BRUCE_OK;

    bool ran = selftest__config_run_as(false, "selftest_config_protected.elf", selftest__config_protected_entry);
    int calls = s_mock.call_count;

    selftest__config_dialog_mock_clear();

    bool ok = ran && s_config_get_result == BRUCE_ERR_PERMISSION && s_config_set_result == BRUCE_ERR_PERMISSION &&
              calls == 0;
    printf("[selftest] config/protected-field-denied: %s (get=%d set=%d dialog_calls=%d)\n", ok ? "OK" : "FAIL",
           s_config_get_result, s_config_set_result, calls);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_builtin_manage_case                                 */
/* ------------------------------------------------------------------------ */

bool selftest__run_config_builtin_manage_case(void)
{
    /* Called directly within selftest's own built-in task context. */
    bruce_result_t set_general = config__set_sound_volume(42);
    int general_value = -1;
    bruce_result_t get_general = config__get_sound_volume(&general_value);

    char original_ssid[33] = {0};
    char original_password[65] = {0};
    bruce_result_t get_original = config__get_wifi_ap(original_ssid, sizeof(original_ssid), original_password,
                                                       sizeof(original_password));
    bruce_result_t set_protected = config__set_wifi_ap("SelftestNet", "selftestpwd");
    char ssid[33] = {0};
    char password[65] = {0};
    bruce_result_t get_protected = config__get_wifi_ap(ssid, sizeof(ssid), password, sizeof(password));
    /* Restore the original AP credentials so this test doesn't permanently
     * change device configuration as a side effect. */
    if (get_original == BRUCE_OK) config__set_wifi_ap(original_ssid, original_password);

    bool ok = set_general == BRUCE_OK && get_general == BRUCE_OK && general_value == 42 && set_protected == BRUCE_OK &&
              get_protected == BRUCE_OK && strcmp(ssid, "SelftestNet") == 0 && strcmp(password, "selftestpwd") == 0;
    printf("[selftest] config/builtin-manage: %s (general=%d ssid=\"%s\")\n", ok ? "OK" : "FAIL", general_value, ssid);
    return ok;
}
