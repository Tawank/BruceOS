#include "wifi_app.h"

#include <stdbool.h>
#include <string.h>

#include "args.h"
#include "core_sdk/stdio.h"
#include "core_sdk/wifi.h"

static int wifi_app_default(void) { return wifi__connect_known() == BRUCE_OK ? 0 : -1; }

static int wifi_app_scan(void) {
    wifi__network_t networks[32];
    int count = wifi__scan(networks, sizeof(networks) / sizeof(networks[0]));
    if (count < 0) { return -1; }

    for (int i = 0; i < count; ++i) {
        stdio__printf(
            "%s rssi=%d channel=%u auth=%u\n",
            networks[i].ssid,
            networks[i].rssi,
            networks[i].channel,
            networks[i].authmode
        );
    }
    return 0;
}

static int wifi_app_connect(ArgParser *parser) {
    char *ssid = ap_get_arg(parser, "ssid");
    char *password = ap_get_arg(parser, "password");
    if (ssid == NULL && password == NULL) { return wifi__connect_known() == BRUCE_OK ? 0 : -1; }
    if (ssid == NULL || password == NULL) {
        stdio__printf("wifi connect requires ssid and password\n");
        return -1;
    }

    if (wifi__connect(ssid, password, 10000) != BRUCE_OK) { return -1; }
    return wifi__add_credential(ssid, password) == BRUCE_OK ? 0 : -1;
}

static int wifi_app_ap_info(void) {
    if (!wifi__is_ap_running()) {
        stdio__printf("Wi-Fi AP is not running\n");
        return -1;
    }
    const char *ssid = wifi__get_ssid();
    const char *ip = wifi__get_ip();
    stdio__printf("SSID: %s\nIP: %s\n", ssid != NULL ? ssid : "unknown", ip != NULL ? ip : "unknown");
    return 0;
}

static int wifi_app_add(ArgParser *parser) {
    char *ssid = ap_get_arg(parser, "ssid");
    char *password = ap_get_arg(parser, "password");
    return ssid != NULL && password != NULL && wifi__add_credential(ssid, password) == BRUCE_OK ? 0 : -1;
}

static int wifi_app_webui(ArgParser *parser) {
    char *mode = ap_get_arg(parser, "mode");
    bool no_ap = mode != NULL && strcmp(mode, "noAp") == 0;
    (void)no_ap;
    stdio__printf("wifi webui is unavailable\n");
    return -1;
}

static void wifi_app_add_common_options(ArgParser *parser) {
    ap_add_flag(parser, "gui");
    ap_set_opt_help(parser, "gui", "Use GUI interaction mode");
}

int wifi_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return -1;
    ap_set_helptext(root, "Manage Wi-Fi station and access-point modes.");
    wifi_app_add_common_options(root);

    ArgParser *on = ap_new_cmd(root, "on");
    ArgParser *off = ap_new_cmd(root, "off disconnect");
    ArgParser *add = ap_new_cmd(root, "add");
    ArgParser *ap = ap_new_cmd(root, "ap");
    ArgParser *webui = ap_new_cmd(root, "webui");
    ArgParser *arp = ap_new_cmd(root, "arp");
    ArgParser *listen = ap_new_cmd(root, "listen");
    ArgParser *sniffer = ap_new_cmd(root, "sniffer");
    ArgParser *scan = ap_new_cmd(root, "scan");
    ArgParser *connect = ap_new_cmd(root, "connect");
    ArgParser *ap_start = ap != NULL ? ap_new_cmd(ap, "start") : NULL;
    ArgParser *ap_info = ap != NULL ? ap_new_cmd(ap, "info") : NULL;

    ArgParser *commands[] = {on, off, add, ap, webui, arp, listen, sniffer, scan, connect, ap_start, ap_info};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (commands[i] == NULL) {
            ap_free(root);
            return -1;
        }
        wifi_app_add_common_options(commands[i]);
    }

    ap_set_helptext(on, "Connect using saved credentials, or start the configured AP.");
    ap_set_helptext(off, "Disconnect Wi-Fi.");
    ap_set_helptext(add, "Save a Wi-Fi credential.");
    ap_add_required_arg(add, "ssid", "Network name");
    ap_add_required_arg(add, "password", "Network password");
    ap_unknown_options_as_args(add);
    ap_set_helptext(ap, "Manage access-point mode.");
    ap_set_helptext(ap_start, "Start the configured access point.");
    ap_set_helptext(ap_info, "Show access-point status and addresses.");
    ap_set_helptext(webui, "Start the Web UI when available.");
    ap_add_optional_arg(webui, "mode", "Use noAp to avoid AP fallback");
    ap_set_helptext(arp, "Scan hosts on the connected network.");
    ap_set_helptext(listen, "Start the Wi-Fi TCP listener.");
    ap_set_helptext(sniffer, "Start Wi-Fi packet capture.");
    ap_set_helptext(scan, "List nearby Wi-Fi networks.");
    ap_set_helptext(connect, "Connect saved credentials or provide a network and password.");
    ap_add_optional_arg(connect, "ssid", "Network name");
    ap_add_optional_arg(connect, "password", "Network password");
    ap_unknown_options_as_args(connect);

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        return status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? 0 : -1;
    }

    int result = -1;
    ArgParser *command = ap_get_cmd_parser(root);
    if (command == NULL) result = wifi_app_default();
    else if (command == on)
        result = wifi__connect_known() == BRUCE_OK ? 0 : wifi__setup_ap() == BRUCE_OK ? 0 : -1;
    else if (command == off) result = wifi__disconnect() == BRUCE_OK ? 0 : -1;
    else if (command == add) result = wifi_app_add(add);
    else if (command == webui) result = wifi_app_webui(webui);
    else if (command == arp) result = wifi__scan_hosts() == BRUCE_OK ? 0 : -1;
    else if (command == listen) result = wifi__listen_tcp() == BRUCE_OK ? 0 : -1;
    else if (command == sniffer) result = wifi__start_sniffer() == BRUCE_OK ? 0 : -1;
    else if (command == scan) result = wifi_app_scan();
    else if (command == connect) result = wifi_app_connect(connect);
    else if (command == ap) {
        ArgParser *ap_command = ap_get_cmd_parser(ap);
        if (ap_command == ap_start) result = wifi__setup_ap() == BRUCE_OK ? 0 : -1;
        else if (ap_command == ap_info) result = wifi_app_ap_info();
        else {
            ap_print_help(ap);
            result = -1;
        }
    }

    ap_free(root);
    return result;
}
