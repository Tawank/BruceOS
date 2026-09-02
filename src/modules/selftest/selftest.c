#include "selftest.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "args.h"
#include "core_sdk/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/process.h"

#include "app_runner_test.h"
#include "archive_test.h"
#include "args_test.h"
#include "audio_test.h"
#include "base64_test.h"
#include "bluetooth_test.h"
#include "bnu_test.h"
#include "clock_test.h"
#include "compress_test.h"
#include "config_test.h"
#include "device_test.h"
#include "dialog_test.h"
#include "display_test.h"
#include "elf_loader_test.h"
#include "filetype_test.h"
#include "gpio_bus_test.h"
#include "hash_test.h"
#include "html_test.h"
#include "icon_test.h"
#include "image_test.h"
#include "input_test.h"
#include "ir_test.h"
#include "launcher_test.h"
#include "loader_test.h"
#include "memory_test.h"
#include "notification_test.h"
#include "nrf24_test.h"
#include "partition_manager_test.h"
#include "clipboard_test.h"
#include "permission_test.h"
#include "storage_test.h"
#include "process_test.h"
#include "shell_test.h"
#include "terminal_test.h"
#include "wasm_bruce_sdk_test.h"
#include "wifi_test.h"

void selftest__resource_cleanup(void *context) {
    selftest__shared_t *shared = (selftest__shared_t *)context;
    shared->resource_cleanup_ran = true;
}

static int selftest__visual_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int failures = 0;
    failures += !selftest__run_display_compositor_case();
    failures += !selftest__run_display_rendering_case();
    failures += !selftest__run_icon_registry_case();
    failures += !selftest__run_image_decode_case();
    failures += !selftest__run_notification_case();
    return failures == 0 ? 0 : 1;
}

static bool selftest__run_visual_cases(void) {
    bruce_result_t registered =
        app_runner__register("selftest_visual", "Visual self-test runner", "Test", selftest__visual_entry, 0);
    if (registered != BRUCE_OK && registered != BRUCE_ERR_ALREADY_EXISTS) return false;

    int launched = app_runner__run_command("GUI=1 selftest_visual", BRUCE_LAUNCH_FOREGROUND);
    bruce_process_status_t status;
    bool ok = launched > 0 && process__wait_status((bruce_process_id_t)launched, 5000, &status) == BRUCE_OK &&
              status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0;
    if (!ok) printf("[selftest] visual: foreground GUI child failed\n");
    return ok;
}

int selftest_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(
        parser, "Run BruceOS's full hardware and Core self-test suite (slow; exercises storage, "
                "partitions, and process management for real)."
    );
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        return status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
               : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                        : BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(parser);

    (void)storage__mkdir("/apps");
    (void)storage__mkdir("/bin");

    int failures = 0;
#define RUN_SELFTEST(fn)                                                                                     \
    do {                                                                                                     \
        bool passed = (fn)();                                                                                \
        printf("[selftest] %s %s\n", #fn, passed ? "PASS" : "FAIL");                                         \
        if (!passed) failures++;                                                                             \
    } while (0)

    RUN_SELFTEST(selftest__run_process_normal_exit_case);
    RUN_SELFTEST(selftest__run_process_status_case);
    RUN_SELFTEST(selftest__run_process_killed_case);
    RUN_SELFTEST(selftest__run_process_clear_signal_case);
    RUN_SELFTEST(selftest__run_process_registry_growth_case);
    RUN_SELFTEST(selftest__run_process_resource_growth_case);
    RUN_SELFTEST(selftest__run_runtime_now_case);
    RUN_SELFTEST(selftest__run_runtime_timer_case);
    RUN_SELFTEST(selftest__run_audio_stream_nonblocking_case);
    RUN_SELFTEST(selftest__run_external_memory_case);
    RUN_SELFTEST(selftest__run_memory_layout_case);
    RUN_SELFTEST(selftest__run_external_memory_xip_case);
    RUN_SELFTEST(selftest__run_external_memory_slab_case);
    RUN_SELFTEST(selftest__run_process_app_switch_case);
    RUN_SELFTEST(selftest__run_process_app_kill_case);
    RUN_SELFTEST(selftest__run_device_state_case);
    RUN_SELFTEST(selftest__run_clock_case);
    RUN_SELFTEST(selftest__run_compress_one_shot_case);
    RUN_SELFTEST(selftest__run_compress_streaming_case);
    RUN_SELFTEST(selftest__run_compress_corrupt_input_case);
    RUN_SELFTEST(selftest__run_compress_invalid_args_case);
    RUN_SELFTEST(selftest__run_archive_tar_gz_roundtrip_case);
    RUN_SELFTEST(selftest__run_archive_zip_roundtrip_case);
    RUN_SELFTEST(selftest__run_archive_tar_gz_corrupt_case);
    RUN_SELFTEST(selftest__run_archive_zip_corrupt_case);
    RUN_SELFTEST(selftest__run_archive_tar_gz_slip_rejection_case);
    RUN_SELFTEST(selftest__run_archive_not_found_case);
    RUN_SELFTEST(selftest__run_apprunner_registration_case);
    RUN_SELFTEST(selftest__run_apprunner_args_case);
    RUN_SELFTEST(selftest__run_apprunner_resolution_case);
    RUN_SELFTEST(selftest__run_args_case);
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
    RUN_SELFTEST(selftest__run_filetype_extension_case);
    RUN_SELFTEST(selftest__run_filetype_magic_bytes_case);
    RUN_SELFTEST(selftest__run_filetype_shebang_case);
    RUN_SELFTEST(selftest__run_filetype_text_binary_case);
    RUN_SELFTEST(selftest__run_filetype_identify_path_case);
    RUN_SELFTEST(selftest__run_partition_manager_default_layout_case);
    RUN_SELFTEST(selftest__run_partition_manager_validation_case);
    RUN_SELFTEST(selftest__run_partition_manager_stage_lifecycle_case);
    RUN_SELFTEST(selftest__run_partition_manager_pending_changes_case);
    RUN_SELFTEST(selftest__run_config_permission_denied_case);
    RUN_SELFTEST(selftest__run_config_permission_allowed_case);
    RUN_SELFTEST(selftest__run_config_protected_field_denied_case);
    RUN_SELFTEST(selftest__run_config_builtin_manage_case);
    RUN_SELFTEST(selftest__run_config_theme_case);
    RUN_SELFTEST(selftest__run_manifest_parse_case);
    RUN_SELFTEST(selftest__run_html_url_case);
    RUN_SELFTEST(selftest__run_html_parser_case);
    RUN_SELFTEST(selftest__run_loader_registry_extensibility_case);
    RUN_SELFTEST(selftest__run_elf_loader_case);
    RUN_SELFTEST(selftest__run_elf_loader_xip_case);
    RUN_SELFTEST(selftest__run_reclaim_handoff_case);
    RUN_SELFTEST(selftest__run_wasm_loader_case);
    RUN_SELFTEST(selftest__run_wasm_manifest_case);
    RUN_SELFTEST(selftest__run_wasm_bruce_abi_case);
    RUN_SELFTEST(selftest__run_js_loader_case);
    RUN_SELFTEST(selftest__run_terminal_named_case);
    RUN_SELFTEST(selftest__run_terminal_path_case);
    RUN_SELFTEST(selftest__run_terminal_invalid_case);
    RUN_SELFTEST(selftest__run_terminal_stdio_case);
    RUN_SELFTEST(selftest__run_terminal_stdio_cancel_case);
    RUN_SELFTEST(selftest__run_terminal_editing_case);
    RUN_SELFTEST(selftest__run_shell_language_case);
    RUN_SELFTEST(selftest__run_shell_script_case);
    RUN_SELFTEST(selftest__run_shell_control_flow_case);
    RUN_SELFTEST(selftest__run_shell_multiline_case);
    RUN_SELFTEST(selftest__run_shell_loops_case);
    RUN_SELFTEST(selftest__run_shell_pipe_redirect_case);
    RUN_SELFTEST(selftest__run_shell_bnu_text_pipe_case);
    RUN_SELFTEST(selftest__run_shell_read_case);
    RUN_SELFTEST(selftest__run_shell_stdio_inheritance_case);
    RUN_SELFTEST(selftest__run_shell_tty_size_case);
    RUN_SELFTEST(selftest__run_shell_interrupt_case);
    RUN_SELFTEST(selftest__run_shell_eof_case);
    RUN_SELFTEST(selftest__run_bnu_case);
    RUN_SELFTEST(selftest__run_bnu_text_case);
    RUN_SELFTEST(selftest__run_bnu_free_stack_case);
    RUN_SELFTEST(selftest__run_launcher_apps_discovery_case);
    RUN_SELFTEST(selftest__run_wifi_permission_denied_case);
    RUN_SELFTEST(selftest__run_http_permission_denied_case);
    RUN_SELFTEST(selftest__run_wifi_http_independent_permission_case);
    RUN_SELFTEST(selftest__run_tcp_permission_denied_case);
    RUN_SELFTEST(selftest__run_ssh_permission_denied_case);
    RUN_SELFTEST(selftest__run_ssh_keygen_case);
    RUN_SELFTEST(selftest__run_ir_permission_denied_case);
    RUN_SELFTEST(selftest__run_ir_validation_case);
    RUN_SELFTEST(selftest__run_audio_validation_case);
    RUN_SELFTEST(selftest__run_audio_kill_mid_tone_case);
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
    RUN_SELFTEST(selftest__run_input_hotkey_duration_case);
    RUN_SELFTEST(selftest__run_input_hotkey_code_name_case);
    RUN_SELFTEST(selftest__run_input_hotkey_find_case);
    RUN_SELFTEST(selftest__run_input_hotkey_emit_case);
    RUN_SELFTEST(selftest__run_bluetooth_hid_keyboard_translation_case);
    RUN_SELFTEST(selftest__run_bluetooth_hid_validation_case);
    RUN_SELFTEST(selftest__run_dialog_text_input_case);
    RUN_SELFTEST(selftest__run_dialog_hex_input_case);
    RUN_SELFTEST(selftest__run_dialog_number_input_case);
    RUN_SELFTEST(selftest__run_dialog_pick_file_case);
    RUN_SELFTEST(selftest__run_dialog_viewer_case);
    RUN_SELFTEST(selftest__run_visual_cases);
    RUN_SELFTEST(selftest__run_notification_console_fallback_case);
    RUN_SELFTEST(selftest__run_status_icon_case);
    RUN_SELFTEST(selftest__run_clipboard_text_case);
    RUN_SELFTEST(selftest__run_clipboard_files_case);
    RUN_SELFTEST(selftest__run_hash_crc32_case);
    RUN_SELFTEST(selftest__run_hash_md5_case);
    RUN_SELFTEST(selftest__run_hash_sha256_case);
    RUN_SELFTEST(selftest__run_base64_encode_case);
    RUN_SELFTEST(selftest__run_base64_decode_case);

#undef RUN_SELFTEST
    printf("[selftest] summary: %d failure(s)\n", failures);
    printf("SELFTEST %s\n", failures == 0 ? "PASS" : "FAIL");
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
