#include "wifi_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/config.h"
#include "core_sdk/notification.h"
#include "core_sdk/stdio.h"
#include "core_sdk/wifi.h"

/* Mirrors wifi__connect()'s own default (core/wifi/wifi_common.c) -- there's
 * no accessor for it, so this is just the upper bound used to size the
 * "Connecting..." banner's lifetime, not a behavioral dependency. */
#define WIFI_APP_CONNECT_TIMEOUT_MS 10000u
/* wifi__connect_known() may try several saved networks in turn (one
 * wifi__connect() attempt each), so give its "Connecting..." banner more
 * headroom than a single explicit connect gets. */
#define WIFI_APP_KNOWN_CONNECT_BANNER_MS 20000u

static const char *wifi_app_result_label(bruce_result_t result) {
    switch (result) {
        case BRUCE_ERR_TIMEOUT: return "timed out";
        case BRUCE_ERR_INVALID_ARGUMENT: return "bad credentials";
        case BRUCE_ERR_PERMISSION: return "not permitted";
        case BRUCE_ERR_NOT_FOUND: return "no saved network in range";
        default: return "failed";
    }
}

/* Announces the start of a connect attempt. Kept as its own push (rather
 * than folded into the result notification) so the user sees the attempt
 * start, not just its outcome -- notification__push() is last-writer-wins,
 * so this is replaced by wifi_app_notify_connect_result() the moment
 * wifi__connect()/wifi__connect_known() returns. How (or whether) this
 * actually reaches the user -- a GUI banner or a console line -- is decided
 * by notification__push()/modules/notification_service, not here. */
static void wifi_app_notify_connecting(uint32_t banner_duration_ms) {
    (void)notification__push("Connecting to Wi-Fi...", banner_duration_ms);
}

static void wifi_app_notify_connect_result(bruce_result_t result) {
    if (result == BRUCE_OK) {
        (void)notification__push("Wi-Fi connected", 3000);
        return;
    }
    char message[BRUCE_NOTIFICATION_TEXT_MAX];
    snprintf(message, sizeof(message), "Wi-Fi connect failed: %s", wifi_app_result_label(result));
    (void)notification__push(message, 4000);
}

static void wifi_app_notify_ap_starting(void) { (void)notification__push("Starting Wi-Fi AP...", 8000); }

static void wifi_app_notify_ap_result(bruce_result_t result) {
    (void)notification__push(result == BRUCE_OK ? "Wi-Fi AP started" : "Wi-Fi AP failed to start", 4000);
}

static int wifi_app_default(void) {
    wifi_app_notify_connecting(WIFI_APP_KNOWN_CONNECT_BANNER_MS);
    bruce_result_t result = wifi__connect_known();
    wifi_app_notify_connect_result(result);
    return result == BRUCE_OK ? 0 : -1;
}

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
    if (ssid == NULL && password == NULL) {
        wifi_app_notify_connecting(WIFI_APP_KNOWN_CONNECT_BANNER_MS);
        bruce_result_t result = wifi__connect_known();
        wifi_app_notify_connect_result(result);
        return result == BRUCE_OK ? 0 : -1;
    }
    if (ssid == NULL || password == NULL) {
        stdio__printf("wifi connect requires ssid and password\n");
        return -1;
    }

    wifi_app_notify_connecting(WIFI_APP_CONNECT_TIMEOUT_MS + 3000);
    bruce_result_t result = wifi__connect(ssid, password, WIFI_APP_CONNECT_TIMEOUT_MS);
    wifi_app_notify_connect_result(result);
    if (result != BRUCE_OK) { return -1; }
    return config__add_or_update_wifi_credential(ssid, password) == BRUCE_OK ? 0 : -1;
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
    return ssid != NULL && password != NULL &&
                   config__add_or_update_wifi_credential(ssid, password) == BRUCE_OK
               ? 0
               : -1;
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
    ArgParser *scan = ap_new_cmd(root, "scan");
    ArgParser *connect = ap_new_cmd(root, "connect");
    ArgParser *ap_start = ap != NULL ? ap_new_cmd(ap, "start") : NULL;
    ArgParser *ap_info = ap != NULL ? ap_new_cmd(ap, "info") : NULL;

    ArgParser *commands[] = {on, off, add, ap, scan, connect, ap_start, ap_info};
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
    else if (command == on) {
        wifi_app_notify_connecting(WIFI_APP_KNOWN_CONNECT_BANNER_MS);
        bruce_result_t connect_result = wifi__connect_known();
        if (connect_result == BRUCE_OK) {
            (void)notification__push("Wi-Fi connected", 3000);
            result = 0;
        } else {
            wifi_app_notify_ap_starting();
            bruce_result_t ap_result = wifi__setup_ap();
            wifi_app_notify_ap_result(ap_result);
            result = ap_result == BRUCE_OK ? 0 : -1;
        }
    } else if (command == off) {
        bruce_result_t disconnect_result = wifi__disconnect();
        (void)notification__push(
            disconnect_result == BRUCE_OK ? "Wi-Fi disconnected" : "Wi-Fi disconnect failed", 3000
        );
        result = disconnect_result == BRUCE_OK ? 0 : -1;
    } else if (command == add) result = wifi_app_add(add);
    else if (command == scan) result = wifi_app_scan();
    else if (command == connect) result = wifi_app_connect(connect);
    else if (command == ap) {
        ArgParser *ap_command = ap_get_cmd_parser(ap);
        if (ap_command == ap_start) {
            wifi_app_notify_ap_starting();
            bruce_result_t ap_result = wifi__setup_ap();
            wifi_app_notify_ap_result(ap_result);
            result = ap_result == BRUCE_OK ? 0 : -1;
        } else if (ap_command == ap_info) result = wifi_app_ap_info();
        else {
            ap_print_help(ap);
            result = -1;
        }
    }

    ap_free(root);
    return result;
}
