#include "args_test.h"

#include <stdio.h>
#include <string.h>

#include "args.h"
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
    bool missing_ok = !ap_parse(required, 1, missing_argv) &&
                      ap_get_status(required) == AP_STATUS_INVALID_ARGUMENT;
    ap_free(required);

    ArgParser *help = ap_new_parser();
    if (help == NULL) return false;
    ap_set_helptext(help, "Parser help test.");
    char *help_argv[] = {"example", "--help"};
    bool help_ok = !ap_parse(help, 2, help_argv) && ap_get_status(help) == AP_STATUS_HELP;
    ap_free(help);
    return missing_ok && help_ok;
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
              selftest__args_wifi_integration();
    printf("[selftest] args: %s\n", ok ? "OK" : "failed");
    return ok;
}
