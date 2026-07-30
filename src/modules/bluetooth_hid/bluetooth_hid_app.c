#include "bluetooth_hid_app.h"

#include <stdio.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/bluetooth_hid.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#define BLUETOOTH_HID_APP__MAX_RESULTS 24

static bool bluetooth_hid_app__parse_address(const char *text, uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN]) {
    unsigned int bytes[BRUCE_BLUETOOTH_ADDRESS_LEN];
    int consumed = 0;
    if (text == NULL ||
        sscanf(
            text,
            "%2x:%2x:%2x:%2x:%2x:%2x%n",
            &bytes[0],
            &bytes[1],
            &bytes[2],
            &bytes[3],
            &bytes[4],
            &bytes[5],
            &consumed
        ) != BRUCE_BLUETOOTH_ADDRESS_LEN ||
        text[consumed] != '\0') {
        return false;
    }
    for (size_t i = 0; i < BRUCE_BLUETOOTH_ADDRESS_LEN; ++i) address[i] = (uint8_t)bytes[i];
    return true;
}

static const char *bluetooth_hid_app__usage_name(bluetooth_hid__usage_t usage) {
    if (usage == BRUCE_BLUETOOTH_HID_KEYBOARD) return "keyboard";
    if (usage == BRUCE_BLUETOOTH_HID_GAMEPAD) return "gamepad";
    return "HID";
}

static int bluetooth_hid_app__stay_active(void) {
    bruce_process_snapshot_t snapshot;
    bruce_result_t status = process__snapshot(process__current_id(), &snapshot);
    if (status != BRUCE_OK) return status;
    if (snapshot.state != BRUCE_PROCESS_BACKGROUND) {
        bruce_result_t background = process__to_background();
        if (background != BRUCE_OK) return background;
    }
    while (bluetooth_hid__is_connected()) {
        if (runtime__sleep(250) == BRUCE_ERR_CANCELLED) break;
    }
    (void)bluetooth_hid__disconnect();
    return 0;
}

static int bluetooth_hid_app__connect(const uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN], bool gui) {
    bruce_result_t result = bluetooth_hid__connect(address, 15000);
    if (result != BRUCE_OK) {
        if (gui) {
            char message[64];
            snprintf(message, sizeof(message), "Connection failed (%d)", result);
            (void)dialog__message(BRUCE_DIALOG_ERROR, "Bluetooth HID", message);
        } else {
            stdio__printf("Bluetooth HID connection failed: %d\n", result);
        }
        return result;
    }
    if (gui) {
        (void)dialog__message(
            BRUCE_DIALOG_SUCCESS,
            "Bluetooth HID",
            "Connected. Input is active in Bruce; this adapter will continue in the background."
        );
    } else {
        stdio__printf("Connected. Bluetooth HID input adapter is running in the background.\n");
    }
    return bluetooth_hid_app__stay_active();
}

static int bluetooth_hid_app__scan_terminal(void) {
    bluetooth_hid__device_t devices[BLUETOOTH_HID_APP__MAX_RESULTS];
    int count = bluetooth_hid__scan(devices, BLUETOOTH_HID_APP__MAX_RESULTS, 10000);
    if (count < 0) {
        stdio__printf("Classic Bluetooth HID scan failed: %d\n", count);
        return count;
    }
    for (int i = 0; i < count; ++i) {
        stdio__printf(
            "%02X:%02X:%02X:%02X:%02X:%02X rssi=%d type=%s name=%s\n",
            devices[i].address[0],
            devices[i].address[1],
            devices[i].address[2],
            devices[i].address[3],
            devices[i].address[4],
            devices[i].address[5],
            devices[i].rssi,
            bluetooth_hid_app__usage_name(devices[i].usage),
            devices[i].name[0] != '\0' ? devices[i].name : "(unnamed)"
        );
    }
    return 0;
}

static int bluetooth_hid_app__scan_and_connect_gui(void) {
    bluetooth_hid__device_t devices[BLUETOOTH_HID_APP__MAX_RESULTS];
    int count = bluetooth_hid__scan(devices, BLUETOOTH_HID_APP__MAX_RESULTS, 10000);
    if (count < 0) {
        char message[64];
        snprintf(message, sizeof(message), "Classic HID scan failed (%d)", count);
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Bluetooth HID", message);
        return count;
    }
    if (count == 0) {
        (void)dialog__message(BRUCE_DIALOG_INFO, "Bluetooth HID", "No keyboards or gamepads found");
        return 0;
    }
    char labels[BLUETOOTH_HID_APP__MAX_RESULTS][96];
    bruce_dialog_choice_t choices[BLUETOOTH_HID_APP__MAX_RESULTS];
    for (int i = 0; i < count; ++i) {
        snprintf(
            labels[i],
            sizeof(labels[i]),
            "%s (%s, %d dBm)",
            devices[i].name[0] != '\0' ? devices[i].name : "(unnamed)",
            bluetooth_hid_app__usage_name(devices[i].usage),
            devices[i].rssi
        );
        choices[i].label = labels[i];
        choices[i].value = labels[i];
    }
    size_t selected = 0;
    bruce_result_t choice = dialog__choice(
        "Classic Bluetooth HID", "Select a controller", choices, (size_t)count, &selected, NULL
    );
    return choice == BRUCE_OK ? bluetooth_hid_app__connect(devices[selected].address, true) : 0;
}

static int bluetooth_hid_app__gui(void) {
    if (!bluetooth_hid__is_supported()) {
        (void)dialog__message(
            BRUCE_DIALOG_WARNING, "Bluetooth HID", "Classic Bluetooth is unavailable on this target."
        );
        return BRUCE_ERR_UNSUPPORTED;
    }
    const bruce_dialog_choice_t choices[] = {
        {"Scan and connect", "connect"   },
        {"Disconnect",       "disconnect"},
    };
    size_t selected = 0;
    if (dialog__choice("Bluetooth HID", "Keyboards and gamepads", choices, 2, &selected, NULL) != BRUCE_OK)
        return 0;
    if (selected == 1) return bluetooth_hid__disconnect();
    return bluetooth_hid_app__scan_and_connect_gui();
}

int bluetooth_hid_app_main(int argc, char **argv) {
    if (app_runner__args_have_gui(argc, argv)) {
        if (!app_runner__args_have_background(argc, argv)) {
            bruce_result_t foreground = process__to_foreground();
            if (foreground != BRUCE_OK) return foreground;
        }
        return bluetooth_hid_app__gui();
    }

    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Manage a Classic Bluetooth keyboard or gamepad input adapter.");
    ArgParser *scan = ap_new_cmd(root, "scan");
    ArgParser *connect = ap_new_cmd(root, "connect");
    ArgParser *disconnect = ap_new_cmd(root, "disconnect");
    ArgParser *status = ap_new_cmd(root, "status");
    ap_set_helptext(scan, "List nearby Classic Bluetooth HID devices.");
    ap_set_helptext(connect, "Connect to a Bluetooth HID device.");
    ap_add_required_arg(connect, "address", "Bluetooth address in XX:XX:XX:XX:XX:XX form");
    ap_set_helptext(disconnect, "Disconnect the active Bluetooth HID device.");
    ap_set_helptext(status, "Show the Bluetooth HID connection status.");
    if (!ap_parse(root, argc, argv)) {
        ap_status_t parse_status = ap_get_status(root);
        if (parse_status != AP_STATUS_HELP && parse_status != AP_STATUS_VERSION)
            ap_print_help(ap_get_cmd_parser(root) != NULL ? ap_get_cmd_parser(root) : root);
        int result = parse_status == AP_STATUS_HELP || parse_status == AP_STATUS_VERSION
                         ? BRUCE_OK
                         : parse_status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }
    if (!bluetooth_hid__is_supported()) {
        ap_free(root);
        stdio__printf("Classic Bluetooth HID is unsupported on this target.\n");
        return BRUCE_ERR_UNSUPPORTED;
    }

    int result;
    ArgParser *command = ap_get_cmd_parser(root);
    if (command == NULL || command == scan) {
        ap_free(root);
        result = bluetooth_hid_app__scan_terminal();
    } else if (command == connect) {
        uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN];
        if (!bluetooth_hid_app__parse_address(ap_get_arg(connect, "address"), address)) {
            ap_print_help(connect);
            result = BRUCE_ERR_INVALID_ARGUMENT;
            ap_free(root);
        } else {
            ap_free(root);
            result = bluetooth_hid_app__connect(address, false);
        }
    } else if (command == disconnect) {
        ap_free(root);
        result = bluetooth_hid__disconnect();
    } else {
        ap_free(root);
        stdio__printf(
            "Classic Bluetooth HID: %s\n", bluetooth_hid__is_connected() ? "connected" : "disconnected"
        );
        result = BRUCE_OK;
    }
    return result;
}
