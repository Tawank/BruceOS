#include "args_test.h"

#include <stdio.h>
#include <string.h>

#include "args.h"
#include "modules/bluetooth/bluetooth_app.h"
#include "modules/bluetooth_hid/bluetooth_hid_app.h"
#include "modules/bnu/bnu_app.h"
#include "modules/clock/clock_app.h"
#include "modules/config/config_app.h"
#include "modules/ir/ir_app.h"
#include "modules/loaders/elf/elf_loader_app.h"
#include "modules/loaders/image/image_loader_app.h"
#include "modules/loaders/js/js_loader_app.h"
#include "modules/nrf24/nrf24_app.h"
#include "modules/tcp/tcp_app.h"
#include "modules/utils/notification/notification_app.h"
#include "modules/utils/task/task_app.h"
#include "modules/utils/terminal/terminal_app.h"
#include "modules/webui/webui_app.h"
#include "modules/wifi/wifi_app.h"

static bool selftest__args_named_positionals(void) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return false;
    ArgParser *connect = ap_new_cmd(root, "connect join");
    if (connect == NULL) {
        ap_free(root);
        return false;
    }
    ap_add_optional_arg(connect, "ssid", "Network name");
    ap_add_optional_arg(connect, "password", "Network password");
    ap_add_flag(connect, "gui");
    ap_unknown_options_as_args(connect);

    char *argv[] = {"wifi", "join", "Test Network", "-secret", "--gui"};
    bool parsed = ap_parse(root, 5, argv);
    bool ok = parsed && ap_get_status(root) == AP_STATUS_OK && ap_get_cmd_parser(root) == connect &&
              ap_get_parent(connect) == root && strcmp(ap_get_arg(connect, "ssid"), "Test Network") == 0 &&
              strcmp(ap_get_arg(connect, "password"), "-secret") == 0 &&
              ap_get_arg(connect, "missing") == NULL && ap_found(connect, "gui");
    ap_free(root);
    return ok;
}

static bool selftest__args_nonfatal_status(void) {
    ArgParser *required = ap_new_parser();
    if (required == NULL) return false;
    ap_add_required_arg(required, "value", "Required value");
    char *missing_argv[] = {"required"};
    bool missing_ok =
        !ap_parse(required, 1, missing_argv) && ap_get_status(required) == AP_STATUS_INVALID_ARGUMENT;
    ap_free(required);

    ArgParser *help = ap_new_parser();
    if (help == NULL) return false;
    ap_set_helptext(help, "Parser help test.");
    char *help_argv[] = {"example", "--help"};
    bool help_ok = !ap_parse(help, 2, help_argv) && ap_get_status(help) == AP_STATUS_HELP;
    ap_free(help);
    return missing_ok && help_ok;
}

static bool selftest__args_trailing_positionals(void) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return false;
    ap_add_required_arg(parser, "path", "Input path");
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    char *argv[] = {"loader", "/apps/example.elf", "--literal", "two words"};
    bool parsed = ap_parse(parser, 4, argv);
    bool ok = parsed && strcmp(ap_get_arg(parser, "path"), "/apps/example.elf") == 0 &&
              ap_count_args(parser) == 3 && strcmp(ap_get_arg_at_index(parser, 1), "--literal") == 0 &&
              strcmp(ap_get_arg_at_index(parser, 2), "two words") == 0;
    ap_free(parser);
    return ok;
}

static bool selftest__args_module_help(void) {
    char *argv[] = {"app", "--help"};
    typedef int (*entry_t)(int argc, char **argv);
    static const entry_t entries[] = {
        bluetooth_app_main,
        bluetooth_hid_app_main,
        bnu_pwd_app_main,
        clock_app_main,
        config_app_main,
        ir_app_main,
        elf_loader__app_main,
        image_app_main,
        image_viewer_app_main,
        js_loader__app_main,
        nrf24_app_main,
        notification_app_main,
        task_app_main,
        tcp_app_main,
        terminal_app_main,
        webui_app_main,
        wifi_app_main,
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (entries[i](2, argv) != BRUCE_OK) return false;
    }
    return true;
}

static bool selftest__args_wifi_integration(void) {
    char *help_argv[] = {"wifi", "help", "connect"};
    char *missing_argv[] = {"wifi", "add", "ssid-only"};
    char *unknown_argv[] = {"wifi", "ap", "unknown"};
    return wifi_app_main(3, help_argv) == 0 && wifi_app_main(3, missing_argv) == -1 &&
           wifi_app_main(3, unknown_argv) == -1;
}

bool selftest__run_args_case(void) {
    bool ok = selftest__args_named_positionals() && selftest__args_nonfatal_status() &&
              selftest__args_trailing_positionals() && selftest__args_module_help() &&
              selftest__args_wifi_integration();
    printf("[selftest] args: %s\n", ok ? "OK" : "failed");
    return ok;
}
