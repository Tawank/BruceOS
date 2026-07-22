#include "selftest.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "task_test.h"
#include "app_runner_test.h"
#include "permission_test.h"
#include "storage_test.h"
#include "config_test.h"

void selftest__resource_cleanup(void *context)
{
    selftest__shared_t *shared = (selftest__shared_t *)context;
    shared->resource_cleanup_ran = true;
}

int selftest_app(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("[selftest] %s %s\n", "run_task_normal_exit_case", selftest__run_task_normal_exit_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_task_killed_case", selftest__run_task_killed_case() ? "PASS" : "FAIL");

    printf("[selftest] %s %s\n", "run_apprunner_registration_case", selftest__run_apprunner_registration_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_apprunner_args_case", selftest__run_apprunner_args_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_apprunner_resolution_case", selftest__run_apprunner_resolution_case() ? "PASS" : "FAIL");

    printf("[selftest] %s %s\n", "run_permission_allow_case", selftest__run_permission_allow_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_permission_deny_no_reprompt_case", selftest__run_permission_deny_no_reprompt_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_permission_shared_basename_case", selftest__run_permission_shared_basename_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_permission_builtin_grant_case", selftest__run_permission_builtin_grant_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_permission_preflight_case", selftest__run_permission_preflight_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_dialog_gui_terminal_dispatch_case", selftest__run_dialog_gui_terminal_dispatch_case() ? "PASS" : "FAIL");

    printf("[selftest] %s %s\n", "run_storage_permission_denied_case", selftest__run_storage_permission_denied_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_storage_protected_path_case", selftest__run_storage_protected_path_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_storage_roundtrip_case", selftest__run_storage_roundtrip_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_storage_ownership_case", selftest__run_storage_ownership_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_storage_no_leak_normal_exit_case", selftest__run_storage_no_leak_normal_exit_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_storage_no_leak_killed_case", selftest__run_storage_no_leak_killed_case() ? "PASS" : "FAIL");

    printf("[selftest] %s %s\n", "run_config_permission_denied_case", selftest__run_config_permission_denied_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_config_permission_allowed_case", selftest__run_config_permission_allowed_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_config_protected_field_denied_case", selftest__run_config_protected_field_denied_case() ? "PASS" : "FAIL");
    printf("[selftest] %s %s\n", "run_config_builtin_manage_case", selftest__run_config_builtin_manage_case() ? "PASS" : "FAIL");

    return 0;
}
