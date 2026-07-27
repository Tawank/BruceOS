#include "selftest.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "task_test.h"
#include "bluetooth_test.h"
#include "app_runner_test.h"
#include "permission_test.h"
#include "storage_test.h"
#include "config_test.h"
#include "loader_test.h"
#include "terminal_test.h"
#include "launcher_test.h"
#include "wifi_test.h"
#include "input_test.h"
#include "image_test.h"
#include "ir_test.h"
#include "dialog_test.h"
#include "display_test.h"
#include "gpio_bus_test.h"
#include "notification_test.h"
#include "nrf24_test.h"

void selftest__resource_cleanup(void *context)
{
    selftest__shared_t *shared = (selftest__shared_t *)context;
    shared->resource_cleanup_ran = true;
}

int selftest_app_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int failures = 0;
#define RUN_SELFTEST(fn) do { \
        bool passed = (fn)(); \
        printf("[selftest] %s %s\n", #fn, passed ? "PASS" : "FAIL"); \
        if (!passed) failures++; \
    } while (0)

    RUN_SELFTEST(selftest__run_task_normal_exit_case);
    RUN_SELFTEST(selftest__run_task_killed_case);
    RUN_SELFTEST(selftest__run_runtime_now_case);
    RUN_SELFTEST(selftest__run_apprunner_registration_case);
    RUN_SELFTEST(selftest__run_apprunner_args_case);
    RUN_SELFTEST(selftest__run_apprunner_resolution_case);
    RUN_SELFTEST(selftest__run_permission_allow_case);
    RUN_SELFTEST(selftest__run_permission_deny_no_reprompt_case);
    RUN_SELFTEST(selftest__run_permission_shared_basename_case);
    RUN_SELFTEST(selftest__run_permission_builtin_grant_case);
    RUN_SELFTEST(selftest__run_permission_preflight_case);
    RUN_SELFTEST(selftest__run_permission_protected_boundaries_case);
    RUN_SELFTEST(selftest__run_dialog_gui_terminal_dispatch_case);
    RUN_SELFTEST(selftest__run_storage_permission_denied_case);
    RUN_SELFTEST(selftest__run_storage_protected_path_case);
    RUN_SELFTEST(selftest__run_storage_roundtrip_case);
    RUN_SELFTEST(selftest__run_storage_mkdir_case);
    RUN_SELFTEST(selftest__run_storage_ownership_case);
    RUN_SELFTEST(selftest__run_storage_no_leak_normal_exit_case);
    RUN_SELFTEST(selftest__run_storage_no_leak_killed_case);
    RUN_SELFTEST(selftest__run_config_permission_denied_case);
    RUN_SELFTEST(selftest__run_config_permission_allowed_case);
    RUN_SELFTEST(selftest__run_config_protected_field_denied_case);
    RUN_SELFTEST(selftest__run_config_builtin_manage_case);
    RUN_SELFTEST(selftest__run_manifest_parse_case);
    RUN_SELFTEST(selftest__run_loader_registry_extensibility_case);
    RUN_SELFTEST(selftest__run_elf_loader_case);
    RUN_SELFTEST(selftest__run_js_loader_case);
    RUN_SELFTEST(selftest__run_terminal_named_case);
    RUN_SELFTEST(selftest__run_terminal_path_case);
    RUN_SELFTEST(selftest__run_terminal_invalid_case);
    RUN_SELFTEST(selftest__run_launcher_apps_discovery_case);
    RUN_SELFTEST(selftest__run_wifi_permission_denied_case);
    RUN_SELFTEST(selftest__run_http_permission_denied_case);
    RUN_SELFTEST(selftest__run_wifi_http_independent_permission_case);
    RUN_SELFTEST(selftest__run_tcp_permission_denied_case);
    RUN_SELFTEST(selftest__run_ir_permission_denied_case);
    RUN_SELFTEST(selftest__run_ir_validation_case);
    RUN_SELFTEST(selftest__run_nrf24_permission_denied_case);
    RUN_SELFTEST(selftest__run_nrf24_validation_case);
    RUN_SELFTEST(selftest__run_gpio_bus_permission_denied_case);
    RUN_SELFTEST(selftest__run_gpio_bus_validation_case);
    RUN_SELFTEST(selftest__run_input_poll_case);
    RUN_SELFTEST(selftest__run_input_inject_case);
    RUN_SELFTEST(selftest__run_input_flush_case);
    RUN_SELFTEST(selftest__run_input_non_blocking_case);
    RUN_SELFTEST(selftest__run_input_peek_case);
    RUN_SELFTEST(selftest__run_input_wait_case);
    RUN_SELFTEST(selftest__run_input_check_case);
    RUN_SELFTEST(selftest__run_bluetooth_hid_keyboard_translation_case);
    RUN_SELFTEST(selftest__run_bluetooth_hid_validation_case);
    RUN_SELFTEST(selftest__run_dialog_text_input_case);
    RUN_SELFTEST(selftest__run_dialog_hex_input_case);
    RUN_SELFTEST(selftest__run_dialog_number_input_case);
    RUN_SELFTEST(selftest__run_dialog_pick_file_case);
    RUN_SELFTEST(selftest__run_dialog_viewer_case);
    RUN_SELFTEST(selftest__run_display_compositor_case);
    RUN_SELFTEST(selftest__run_image_decode_case);
    RUN_SELFTEST(selftest__run_notification_case);
    RUN_SELFTEST(selftest__run_status_icon_case);

#undef RUN_SELFTEST
    printf("[selftest] summary: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
