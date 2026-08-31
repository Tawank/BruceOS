#include "wifi_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/notification.h"
#include "core_sdk/runtime.h"
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

/* Networks a single scan can hold on screen at once, and the width of one
 * formatted row ("<ssid> <rssi> dBm [<tag>]"). */
#define WIFI_APP_GUI_SCAN_MAX 24
#define WIFI_APP_GUI_ROW_TEXT 40
/* Mirrors CONFIG__WIFI_PASSWORD_MAX_LEN (core/config/config.h) -- same
 * rationale as WIFI_APP_CONNECT_TIMEOUT_MS above: no public accessor, so
 * this is just the buffer's upper bound. */
#define WIFI_APP_GUI_PASSWORD_MAX 64
/* wifi_ap_record_t.authmode is copied straight through by wifi__scan() (see
 * core/wifi/wifi_common.c). WIFI_AUTH_OPEN is 0 in every ESP-IDF
 * wifi_auth_mode_t Bruce targets, and wifi__network_t exposes only the raw
 * byte (no public enum), so that's the value checked here. */
#define WIFI_APP_GUI_AUTH_OPEN 0u
/* Tick length for the scanning spinner: how long each input__read() wait
 * is, which doubles as the spinner's frame duration and the worst-case
 * latency before a cancel press is noticed. */
#define WIFI_APP_GUI_SCAN_TICK_MS 150u

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

/* "<ssid> <rssi> dBm [<tag>]", tag naming what selecting the row will do:
 * "open" connects straight away, "saved" reuses the stored password (still
 * editable), "locked" prompts for a new one. */
static void wifi_app_gui__format_row(const wifi__network_t *net, bool known, char *out, size_t capacity) {
    const char *tag = net->authmode == WIFI_APP_GUI_AUTH_OPEN ? "open" : (known ? "saved" : "locked");
    snprintf(out, capacity, "%-20.20s %4d dBm [%s]", net->ssid, (int)net->rssi, tag);
}

/* Prompts for a password when the network is secured (pre-filled with the
 * saved one, if any, so confirming as-is just reuses it), connects, and
 * saves the credential on success -- the same save wifi_app_connect() does
 * for an explicit "wifi connect <ssid> <password>". Open networks skip the
 * prompt and connect with an empty password. */
static void wifi_app_gui__connect(const wifi__network_t *net) {
    char password[WIFI_APP_GUI_PASSWORD_MAX + 1] = {0};
    if (net->authmode != WIFI_APP_GUI_AUTH_OPEN) {
        const bruce_config_wifi_credential_t *known = config__find_wifi_credential(net->ssid);
        const char *initial = known != NULL && known->password != NULL ? known->password : "";
        if (dialog__text_input("Wi-Fi password", net->ssid, initial, true, password, sizeof(password)) !=
            BRUCE_OK) {
            return; /* cancelled */
        }
    }

    bruce_result_t result = wifi__connect(net->ssid, password, WIFI_APP_CONNECT_TIMEOUT_MS);
    char message[BRUCE_WIFI_SSID_MAX_LEN + 32];
    if (result == BRUCE_OK) {
        (void)config__add_or_update_wifi_credential(net->ssid, password);
        snprintf(message, sizeof(message), "Connected to %s", net->ssid);
        (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Wi-Fi", message);
    } else {
        snprintf(message, sizeof(message), "%s: %s", net->ssid, wifi_app_result_label(result));
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Wi-Fi", message);
    }
}

/* Runs a Wi-Fi scan behind a full-screen spinner, ticking every
 * WIFI_APP_GUI_SCAN_TICK_MS so the screen animates and Back/B is noticed
 * within about one tick, instead of the old single blocking wifi__scan()
 * call leaving the screen frozen (and uncancellable) for the scan's whole
 * duration. Returns the same thing wifi__scan() does -- a non-negative
 * count or a negative BRUCE_ERR_* value -- plus BRUCE_ERR_CANCELLED if the
 * user backed out. */
static int wifi_app__gui_scan(wifi__network_t *networks, size_t capacity) {
    bruce_result_t start = wifi__scan_start();
    if (start != BRUCE_OK) return (int)start;

    static const char *const frames[] = {"|", "/", "-", "\\"};
    size_t frame = 0;
    for (;;) {
        if (display__begin_frame() == BRUCE_OK) {
            char status[32];
            snprintf(
                status, sizeof(status), "Scanning %s", frames[frame % (sizeof(frames) / sizeof(frames[0]))]
            );
            (void)display__fill_screen(BRUCE_COLOR_BLACK);
            (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
            (void)display__set_text_color(BRUCE_COLOR_WHITE);
            (void)display__set_text_size(2);
            (void)display__draw_centre_string(status, display__width() / 2, (display__height() / 2) - 12);
            (void)display__set_text_size(1);
            (void)display__draw_centre_string("Back to cancel", display__width() / 2, (display__height() / 2) + 16);
            (void)display__present();
        }
        frame++;

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, WIFI_APP_GUI_SCAN_TICK_MS);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND ||
            (input_result == BRUCE_OK && event.action == BRUCE_INPUT_PRESS &&
             (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B))) {
            (void)wifi__scan_cancel();
            return BRUCE_ERR_CANCELLED;
        }

        /* Tick already spent WIFI_APP_GUI_SCAN_TICK_MS waiting on input, so
         * this just checks whether that wait was also long enough for the
         * scan to finish -- no need to wait again. */
        int result = wifi__scan_poll(networks, capacity, 0);
        if (result != BRUCE_ERR_TIMEOUT) return result;
    }
}

/* Interactive picker: scan, list every visible network with its saved/open/
 * locked state, connect on selection, and rescan so the list (and any
 * change the connect attempt caused) is current again. Hidden networks
 * (blank SSID) are left out -- there is nothing to select or connect to by
 * name. */
static int wifi_app__gui(void) {
    for (;;) {
        wifi__network_t networks[WIFI_APP_GUI_SCAN_MAX];
        int count = wifi_app__gui_scan(networks, WIFI_APP_GUI_SCAN_MAX);
        if (count == BRUCE_ERR_CANCELLED) return 0;
        if (count < 0) {
            (void)dialog__message(BRUCE_DIALOG_ERROR, "Wi-Fi scan", "Scan failed.");
            return -1;
        }

        int visible = 0;
        for (int i = 0; i < count; ++i) {
            if (networks[i].ssid[0] == '\0') continue;
            if (visible != i) networks[visible] = networks[i];
            visible++;
        }
        count = visible;

        char rows[WIFI_APP_GUI_SCAN_MAX][WIFI_APP_GUI_ROW_TEXT];
        bruce_dialog_choice_t choices[WIFI_APP_GUI_SCAN_MAX + 2] = {0};
        for (int i = 0; i < count; ++i) {
            bool known = config__find_wifi_credential(networks[i].ssid) != NULL;
            wifi_app_gui__format_row(&networks[i], known, rows[i], sizeof(rows[i]));
            choices[i].label = rows[i];
            choices[i].value = rows[i];
            choices[i].icon_name = "wifi";
            choices[i].right_text = NULL;
        }
        size_t rescan_index = (size_t)count;
        size_t exit_index = rescan_index + 1;
        choices[rescan_index].label = "Rescan";
        choices[rescan_index].value = "rescan";
        choices[rescan_index].icon_name = NULL;
        choices[rescan_index].right_text = NULL;
        choices[exit_index].label = "Exit";
        choices[exit_index].value = "exit";
        choices[exit_index].icon_name = NULL;
        choices[exit_index].right_text = NULL;

        char subtitle[32];
        if (count == 0) snprintf(subtitle, sizeof(subtitle), "No networks found");
        else snprintf(subtitle, sizeof(subtitle), "%d network%s found", count, count == 1 ? "" : "s");

        (void)notification__push("Wi-Fi scan ended", 1000);
        size_t selected = 0;
        bruce_result_t result =
            dialog__choice_launcher("Wi-Fi networks", subtitle, choices, exit_index + 1, &selected);
        if (result == BRUCE_ERR_CANCELLED) return 0;
        if (result != BRUCE_OK) return -1;
        if (strcmp(choices[selected].value, "exit") == 0) return 0;
        if (strcmp(choices[selected].value, "rescan") == 0) continue;

        wifi_app_gui__connect(&networks[selected]);
    }
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

static int wifi_app_ap_start(void) {
    wifi_app_notify_ap_starting();
    bruce_result_t ap_result = wifi__setup_ap();
    wifi_app_notify_ap_result(ap_result);
    return ap_result == BRUCE_OK ? 0 : -1;
}

/* Saved network first, configured AP as the fallback -- "wifi on" means "get
 * this device on a network somehow", not "join a network". */
static int wifi_app_on(void) {
    wifi_app_notify_connecting(WIFI_APP_KNOWN_CONNECT_BANNER_MS);
    if (wifi__connect_known() == BRUCE_OK) {
        (void)notification__push("Wi-Fi connected", 3000);
        return 0;
    }
    return wifi_app_ap_start();
}

/* wifi__disconnect() tears the whole driver down, station and AP alike (see
 * core/wifi/wifi_common.c), so this is also how the AP is stopped. */
static int wifi_app_off(const char *notice) {
    bruce_result_t disconnect_result = wifi__disconnect();
    (void)notification__push(disconnect_result == BRUCE_OK ? notice : "Wi-Fi disconnect failed", 3000);
    return disconnect_result == BRUCE_OK ? 0 : -1;
}

int wifi_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return -1;
    ap_set_helptext(root, "Manage Wi-Fi station and access-point modes.");
    wifi_app_add_common_options(root);

    ArgParser *on = ap_new_cmd(root, "on");
    ArgParser *off = ap_new_cmd(root, "off disconnect");
    ArgParser *toggle = ap_new_cmd(root, "toggle");
    ArgParser *add = ap_new_cmd(root, "add");
    ArgParser *ap = ap_new_cmd(root, "ap");
    ArgParser *scan = ap_new_cmd(root, "scan");
    ArgParser *connect = ap_new_cmd(root, "connect");
    ArgParser *ap_start = ap != NULL ? ap_new_cmd(ap, "start") : NULL;
    ArgParser *ap_toggle = ap != NULL ? ap_new_cmd(ap, "toggle") : NULL;
    ArgParser *ap_info = ap != NULL ? ap_new_cmd(ap, "info") : NULL;

    ArgParser *commands[] = {on, off, toggle, add, ap, scan, connect, ap_start, ap_toggle, ap_info};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (commands[i] == NULL) {
            ap_free(root);
            return -1;
        }
        wifi_app_add_common_options(commands[i]);
    }

    ap_set_helptext(on, "Connect using saved credentials, or start the configured AP.");
    ap_set_helptext(off, "Disconnect Wi-Fi.");
    ap_set_helptext(toggle, "Toggle Wi-Fi state.");
    ap_set_helptext(add, "Save a Wi-Fi credential.");
    ap_add_required_arg(add, "ssid", "Network name");
    ap_add_required_arg(add, "password", "Network password");
    ap_unknown_options_as_args(add);
    ap_set_helptext(ap, "Manage access-point mode.");
    ap_set_helptext(ap_start, "Start the configured access point.");
    ap_set_helptext(ap_toggle, "Toggle the configured access point.");
    ap_set_helptext(ap_info, "Show access-point status and addresses.");
    ap_set_helptext(scan, "List nearby Wi-Fi networks (interactive picker in GUI mode).");
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
    else if (command == on) result = wifi_app_on();
    else if (command == off) result = wifi_app_off("Wi-Fi disconnected");
    else if (command == toggle)
        result = wifi__is_connected() ? wifi_app_off("Wi-Fi disconnected") : wifi_app_on();
    else if (command == add) result = wifi_app_add(add);
    else if (command == scan) result = runtime__gui_requested() ? wifi_app__gui() : wifi_app_scan();
    else if (command == connect) result = wifi_app_connect(connect);
    else if (command == ap) {
        ArgParser *ap_command = ap_get_cmd_parser(ap);
        if (ap_command == ap_start) result = wifi_app_ap_start();
        else if (ap_command == ap_info) result = wifi_app_ap_info();
        else if (ap_command == ap_toggle) {
            result = wifi__is_ap_running() ? wifi_app_off("Wi-Fi AP stopped") : wifi_app_ap_start();
        } else {
            ap_print_help(ap);
            result = -1;
        }
    }

    ap_free(root);
    return result;
}
