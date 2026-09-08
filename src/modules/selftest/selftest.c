#include "selftest.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "args.h"
#include "core_sdk/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/process.h"

#include "app_config_test.h"
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
#include "core_launcher_test.h"
#include "device_test.h"
#include "dialog_test.h"
#include "display_test.h"
#include "elf_loader_test.h"
#include "filemanager_network_test.h"
#include "filemanager_pathicons_test.h"
#include "filetype_test.h"
#include "gpio_bus_test.h"
#include "hash_test.h"
#include "html_test.h"
#include "icmp_test.h"
#include "icon_test.h"
#include "image_test.h"
#include "input_test.h"
#include "ir_test.h"
#include "launcher_test.h"
#include "loader_test.h"
#include "memory_test.h"
#include "net_test.h"
#include "notification_test.h"
#include "nrf24_test.h"
#include "partition_manager_test.h"
#include "clipboard_test.h"
#include "permission_test.h"
#include "storage_test.h"
#include "process_test.h"
#include "shell_test.h"
#include "ssh_sftp_test.h"
#include "terminal_test.h"
#include "udp_test.h"
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

typedef struct {
    const char *name;
    bool (*fn)(void);
} selftest__case_t;

#define SELFTEST_CASE(fn) {#fn, (fn)}

/* One row per self-test case, in the order they have always run in. Filtering
 * (see selftest_app_main() below) walks this table instead of the unconditional,
 * hand-written call list it replaces, so `selftest <filter>...` can run a
 * subset without a second, separately-maintained list to keep in sync. */
static const selftest__case_t selftest__cases[] = {
    SELFTEST_CASE(selftest__run_process_normal_exit_case),
    SELFTEST_CASE(selftest__run_process_status_case),
    SELFTEST_CASE(selftest__run_process_killed_case),
    SELFTEST_CASE(selftest__run_process_clear_signal_case),
    SELFTEST_CASE(selftest__run_process_registry_growth_case),
    SELFTEST_CASE(selftest__run_process_resource_growth_case),
    SELFTEST_CASE(selftest__run_runtime_now_case),
    SELFTEST_CASE(selftest__run_runtime_timer_case),
    SELFTEST_CASE(selftest__run_audio_stream_nonblocking_case),
    SELFTEST_CASE(selftest__run_external_memory_case),
    SELFTEST_CASE(selftest__run_memory_layout_case),
    SELFTEST_CASE(selftest__run_external_memory_xip_case),
    SELFTEST_CASE(selftest__run_external_memory_slab_case),
    SELFTEST_CASE(selftest__run_process_app_switch_case),
    SELFTEST_CASE(selftest__run_process_app_kill_case),
    SELFTEST_CASE(selftest__run_device_state_case),
    SELFTEST_CASE(selftest__run_clock_case),
    SELFTEST_CASE(selftest__run_compress_one_shot_case),
    SELFTEST_CASE(selftest__run_compress_streaming_case),
    SELFTEST_CASE(selftest__run_compress_corrupt_input_case),
    SELFTEST_CASE(selftest__run_compress_invalid_args_case),
    SELFTEST_CASE(selftest__run_archive_tar_gz_roundtrip_case),
    SELFTEST_CASE(selftest__run_archive_zip_roundtrip_case),
    SELFTEST_CASE(selftest__run_archive_tar_gz_corrupt_case),
    SELFTEST_CASE(selftest__run_archive_zip_corrupt_case),
    SELFTEST_CASE(selftest__run_archive_tar_gz_slip_rejection_case),
    SELFTEST_CASE(selftest__run_archive_not_found_case),
    SELFTEST_CASE(selftest__run_archive_tar_gz_entry_ops_case),
    SELFTEST_CASE(selftest__run_archive_zip_entry_ops_case),
    SELFTEST_CASE(selftest__run_apprunner_registration_case),
    SELFTEST_CASE(selftest__run_apprunner_args_case),
    SELFTEST_CASE(selftest__run_apprunner_resolution_case),
    SELFTEST_CASE(selftest__run_args_case),
    SELFTEST_CASE(selftest__run_permission_allow_case),
    SELFTEST_CASE(selftest__run_permission_deny_no_reprompt_case),
    SELFTEST_CASE(selftest__run_permission_shared_basename_case),
    SELFTEST_CASE(selftest__run_permission_builtin_grant_case),
    SELFTEST_CASE(selftest__run_permission_preflight_case),
    SELFTEST_CASE(selftest__run_permission_protected_boundaries_case),
    SELFTEST_CASE(selftest__run_dialog_gui_terminal_dispatch_case),
    SELFTEST_CASE(selftest__run_storage_permission_denied_case),
    SELFTEST_CASE(selftest__run_storage_protected_path_case),
    SELFTEST_CASE(selftest__run_storage_roundtrip_case),
    SELFTEST_CASE(selftest__run_storage_truncate_case),
    SELFTEST_CASE(selftest__run_storage_mkdir_case),
    SELFTEST_CASE(selftest__run_storage_ownership_case),
    SELFTEST_CASE(selftest__run_storage_no_leak_normal_exit_case),
    SELFTEST_CASE(selftest__run_storage_no_leak_killed_case),
    SELFTEST_CASE(selftest__run_filetype_extension_case),
    SELFTEST_CASE(selftest__run_filetype_magic_bytes_case),
    SELFTEST_CASE(selftest__run_filetype_shebang_case),
    SELFTEST_CASE(selftest__run_filetype_text_binary_case),
    SELFTEST_CASE(selftest__run_filetype_identify_path_case),
    SELFTEST_CASE(selftest__run_partition_manager_default_layout_case),
    SELFTEST_CASE(selftest__run_partition_manager_validation_case),
    SELFTEST_CASE(selftest__run_partition_manager_stage_lifecycle_case),
    SELFTEST_CASE(selftest__run_partition_manager_pending_changes_case),
    SELFTEST_CASE(selftest__run_app_config_self_identity_case),
    SELFTEST_CASE(selftest__run_app_config_cross_app_permission_case),
    SELFTEST_CASE(selftest__run_config_permission_denied_case),
    SELFTEST_CASE(selftest__run_config_permission_allowed_case),
    SELFTEST_CASE(selftest__run_config_protected_field_denied_case),
    SELFTEST_CASE(selftest__run_config_builtin_manage_case),
    SELFTEST_CASE(selftest__run_config_theme_case),
    SELFTEST_CASE(selftest__run_manifest_parse_case),
    SELFTEST_CASE(selftest__run_manifest_parse_named_icon_case),
    SELFTEST_CASE(selftest__run_html_url_case),
    SELFTEST_CASE(selftest__run_html_parser_case),
    SELFTEST_CASE(selftest__run_loader_registry_extensibility_case),
    SELFTEST_CASE(selftest__run_elf_loader_case),
    SELFTEST_CASE(selftest__run_elf_loader_xip_case),
    SELFTEST_CASE(selftest__run_elf_loader_stdio_case),
    SELFTEST_CASE(selftest__run_elf_loader_libc_case),
    SELFTEST_CASE(selftest__run_elf_loader_time_case),
    SELFTEST_CASE(selftest__run_elf_loader_posix_case),
    SELFTEST_CASE(selftest__run_elf_loader_socket_case),
    SELFTEST_CASE(selftest__run_elf_loader_exit_case),
    SELFTEST_CASE(selftest__run_reclaim_handoff_case),
    SELFTEST_CASE(selftest__run_wasm_loader_case),
    SELFTEST_CASE(selftest__run_wasm_manifest_case),
    SELFTEST_CASE(selftest__run_wasm_bruce_abi_case),
    SELFTEST_CASE(selftest__run_js_loader_case),
    SELFTEST_CASE(selftest__run_terminal_named_case),
    SELFTEST_CASE(selftest__run_terminal_path_case),
    SELFTEST_CASE(selftest__run_terminal_invalid_case),
    SELFTEST_CASE(selftest__run_terminal_stdio_case),
    SELFTEST_CASE(selftest__run_terminal_stdio_cancel_case),
    SELFTEST_CASE(selftest__run_terminal_editing_case),
    SELFTEST_CASE(selftest__run_terminal_ansi_escapes_case),
    SELFTEST_CASE(selftest__run_shell_language_case),
    SELFTEST_CASE(selftest__run_shell_script_case),
    SELFTEST_CASE(selftest__run_shell_control_flow_case),
    SELFTEST_CASE(selftest__run_shell_local_case),
    SELFTEST_CASE(selftest__run_shell_command_substitution_case),
    SELFTEST_CASE(selftest__run_shell_arith_word_case),
    SELFTEST_CASE(selftest__run_shell_multiline_case),
    SELFTEST_CASE(selftest__run_shell_loops_case),
    SELFTEST_CASE(selftest__run_shell_case_case),
    SELFTEST_CASE(selftest__run_shell_glob_case),
    SELFTEST_CASE(selftest__run_shell_brace_case),
    SELFTEST_CASE(selftest__run_shell_pipe_redirect_case),
    SELFTEST_CASE(selftest__run_shell_output_redirect_case),
    SELFTEST_CASE(selftest__run_shell_builtin_redirect_case),
    SELFTEST_CASE(selftest__run_shell_input_redirect_case),
    SELFTEST_CASE(selftest__run_shell_arith_redirect_case),
    SELFTEST_CASE(selftest__run_shell_heredoc_case),
    SELFTEST_CASE(selftest__run_shell_cat_interactive_case),
    SELFTEST_CASE(selftest__run_shell_bnu_text_pipe_case),
    SELFTEST_CASE(selftest__run_shell_read_case),
    SELFTEST_CASE(selftest__run_shell_stdio_inheritance_case),
    SELFTEST_CASE(selftest__run_shell_tty_size_case),
    SELFTEST_CASE(selftest__run_shell_interrupt_case),
    SELFTEST_CASE(selftest__run_shell_eof_case),
    SELFTEST_CASE(selftest__run_shell_jobs_case),
    SELFTEST_CASE(selftest__run_bnu_case),
    SELFTEST_CASE(selftest__run_bnu_text_case),
    SELFTEST_CASE(selftest__run_bnu_free_stack_case),
    SELFTEST_CASE(selftest__run_launcher_apps_discovery_case),
    SELFTEST_CASE(selftest__run_wifi_permission_denied_case),
    SELFTEST_CASE(selftest__run_http_permission_denied_case),
    SELFTEST_CASE(selftest__run_wifi_http_independent_permission_case),
    SELFTEST_CASE(selftest__run_tcp_permission_denied_case),
    SELFTEST_CASE(selftest__run_udp_permission_denied_case),
    SELFTEST_CASE(selftest__run_udp_loopback_case),
    SELFTEST_CASE(selftest__run_icmp_permission_denied_case),
    SELFTEST_CASE(selftest__run_icmp_loopback_case),
    SELFTEST_CASE(selftest__run_ssh_permission_denied_case),
    SELFTEST_CASE(selftest__run_ssh_keygen_case),
    SELFTEST_CASE(selftest__run_sftp_host_pattern_case),
    SELFTEST_CASE(selftest__run_sftp_config_value_case),
    SELFTEST_CASE(selftest__run_sftp_port_case),
    SELFTEST_CASE(selftest__run_sftp_known_hosts_format_case),
    SELFTEST_CASE(selftest__run_sftp_split_directive_case),
    SELFTEST_CASE(selftest__run_sftp_resolve_identity_path_case),
    SELFTEST_CASE(selftest__run_filemanager_network_provider_parse_case),
    SELFTEST_CASE(selftest__run_filemanager_network_sanitize_name_case),
    SELFTEST_CASE(selftest__run_filemanager_network_split_entry_name_case),
    SELFTEST_CASE(selftest__run_filemanager_pathicons_parse_case),
    SELFTEST_CASE(selftest__run_filemanager_pathicons_match_case),
    SELFTEST_CASE(selftest__run_core_launcher_build_entry_key_case),
    SELFTEST_CASE(selftest__run_core_launcher_label_from_key_case),
    SELFTEST_CASE(selftest__run_core_launcher_json_has_command_case),
    SELFTEST_CASE(selftest__run_core_launcher_json_menu_labels_case),
    SELFTEST_CASE(selftest__run_core_launcher_json_find_menu_case),
    SELFTEST_CASE(selftest__run_ir_permission_denied_case),
    SELFTEST_CASE(selftest__run_ir_validation_case),
    SELFTEST_CASE(selftest__run_audio_validation_case),
    SELFTEST_CASE(selftest__run_audio_kill_mid_tone_case),
    SELFTEST_CASE(selftest__run_nrf24_permission_denied_case),
    SELFTEST_CASE(selftest__run_nrf24_validation_case),
    SELFTEST_CASE(selftest__run_gpio_bus_permission_denied_case),
    SELFTEST_CASE(selftest__run_gpio_bus_validation_case),
    SELFTEST_CASE(selftest__run_input_poll_case),
    SELFTEST_CASE(selftest__run_input_inject_case),
    SELFTEST_CASE(selftest__run_input_flush_case),
    SELFTEST_CASE(selftest__run_input_non_blocking_case),
    SELFTEST_CASE(selftest__run_input_peek_case),
    SELFTEST_CASE(selftest__run_input_wait_case),
    SELFTEST_CASE(selftest__run_input_check_case),
    SELFTEST_CASE(selftest__run_input_hotkey_duration_case),
    SELFTEST_CASE(selftest__run_input_hotkey_code_name_case),
    SELFTEST_CASE(selftest__run_input_hotkey_find_case),
    SELFTEST_CASE(selftest__run_input_hotkey_emit_case),
    SELFTEST_CASE(selftest__run_bluetooth_hid_keyboard_translation_case),
    SELFTEST_CASE(selftest__run_bluetooth_hid_validation_case),
    SELFTEST_CASE(selftest__run_dialog_text_input_case),
    SELFTEST_CASE(selftest__run_dialog_hex_input_case),
    SELFTEST_CASE(selftest__run_dialog_number_input_case),
    SELFTEST_CASE(selftest__run_dialog_pick_file_case),
    SELFTEST_CASE(selftest__run_dialog_viewer_case),
    SELFTEST_CASE(selftest__run_dialog_message_show_case),
    SELFTEST_CASE(selftest__run_dialog_choice_poll_case),
    SELFTEST_CASE(selftest__run_visual_cases),
    SELFTEST_CASE(selftest__run_notification_console_fallback_case),
    SELFTEST_CASE(selftest__run_status_icon_case),
    SELFTEST_CASE(selftest__run_clipboard_text_case),
    SELFTEST_CASE(selftest__run_clipboard_files_case),
    SELFTEST_CASE(selftest__run_hash_crc32_case),
    SELFTEST_CASE(selftest__run_hash_md5_case),
    SELFTEST_CASE(selftest__run_hash_sha256_case),
    SELFTEST_CASE(selftest__run_base64_encode_case),
    SELFTEST_CASE(selftest__run_base64_decode_case),
    SELFTEST_CASE(selftest__run_net_ipv4_case),
};
#define SELFTEST_CASE_COUNT (sizeof(selftest__cases) / sizeof(selftest__cases[0]))

/* Case-insensitive substring search: newlib/picolibc have strcasecmp but no
 * strcasestr, and this is the only place selftest.c needs one. */
static bool selftest__contains_ci(const char *name, const char *filter) {
    size_t name_len = strlen(name);
    size_t filter_len = strlen(filter);
    if (filter_len == 0) return true;
    if (filter_len > name_len) return false;
    for (size_t i = 0; i + filter_len <= name_len; ++i) {
        if (strncasecmp(name + i, filter, filter_len) == 0) return true;
    }
    return false;
}

/* A case runs with no filters given at all (a bare `selftest` still runs the
 * full suite, unchanged), or if its name contains any one of them -- e.g.
 * `selftest args notification` runs every case mentioning either "args" or
 * "notification". Cases aren't otherwise grouped by source module, so a
 * substring against the function name is the closest thing to "just one
 * module" this table offers; `selftest --list` prints the names to filter
 * on. */
static bool selftest__case_selected(const char *name, char *const *filters, int filter_count) {
    if (filter_count <= 0) return true;
    for (int i = 0; i < filter_count; ++i) {
        if (selftest__contains_ci(name, filters[i])) return true;
    }
    return false;
}

int selftest_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(
        parser, "Run BruceOS's full hardware and Core self-test suite (slow; exercises storage, "
                "partitions, and process management for real). With one or more <filter> "
                "arguments, only runs cases whose name contains one of them (case-insensitive), "
                "e.g. `selftest args notification`; see --list for the names to filter on."
    );
    ap_add_flag(parser, "list");
    ap_set_opt_help(parser, "list", "Print case names instead of running them");
    ap_allow_extra_args(parser);
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        return status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
               : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                        : BRUCE_ERR_INVALID_ARGUMENT;
    }

    if (ap_found(parser, "list")) {
        for (size_t i = 0; i < SELFTEST_CASE_COUNT; ++i) printf("%s\n", selftest__cases[i].name);
        ap_free(parser);
        return BRUCE_OK;
    }

    int filter_count = ap_count_args(parser);
    char **filters = filter_count > 0 ? ap_get_args(parser) : NULL;

    (void)storage__mkdir("/apps");
    (void)storage__mkdir("/bin");

    int failures = 0;
    int ran = 0;
    for (size_t i = 0; i < SELFTEST_CASE_COUNT; ++i) {
        if (!selftest__case_selected(selftest__cases[i].name, filters, filter_count)) continue;
        ran++;
        bool passed = selftest__cases[i].fn();
        printf("[selftest] %s %s\n", selftest__cases[i].name, passed ? "PASS" : "FAIL");
        if (!passed) failures++;
    }
    ap_free(parser);

    if (ran == 0 && filter_count > 0) {
        printf("[selftest] no case matched the given filter(s)\n");
        printf("SELFTEST FAIL\n");
        fflush(stdout);
        return 1;
    }
    printf("[selftest] summary: %d failure(s)\n", failures);
    printf("SELFTEST %s\n", failures == 0 ? "PASS" : "FAIL");
    fflush(stdout);
    return failures == 0 ? 0 : 1;
}
