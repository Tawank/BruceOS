#include "bluetooth_app.h"

#include <stdio.h>

#include "args.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/dialog.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define BLUETOOTH_APP__MAX_RESULTS 32

static bool bluetooth_app__resume_after_handoff(void) {
    bruce_process_snapshot_t snapshot;
    bruce_process_id_t self = process__current_id();
    if (self == BRUCE_PROCESS_ID_INVALID || process__snapshot(self, &snapshot) != BRUCE_OK ||
        snapshot.state != BRUCE_PROCESS_BACKGROUND) {
        return false;
    }
    do {
        if (runtime__delay(20) != BRUCE_OK || process__snapshot(self, &snapshot) != BRUCE_OK) return false;
    } while (snapshot.state == BRUCE_PROCESS_BACKGROUND);
    return snapshot.state == BRUCE_PROCESS_FOREGROUND;
}

static bruce_result_t
bluetooth_app__message(bruce_dialog_kind_t type, const char *title, const char *message) {
    bruce_result_t result;
    do {
        result = dialog__message(type, title, message);
    } while (result == BRUCE_ERR_CANCELLED && bluetooth_app__resume_after_handoff());
    return result;
}

static void bluetooth_app__print_address(const uint8_t *address) {
    stdio__printf(
        "%02X:%02X:%02X:%02X:%02X:%02X",
        address[0],
        address[1],
        address[2],
        address[3],
        address[4],
        address[5]
    );
}

static int bluetooth_app__scan_terminal(void) {
    bluetooth__device_t devices[BLUETOOTH_APP__MAX_RESULTS];
    int count = bluetooth__scan_ble(devices, BLUETOOTH_APP__MAX_RESULTS, 5000);
    if (count < 0) {
        stdio__printf("BLE scan failed: %d\n", count);
        return count;
    }
    for (int i = 0; i < count; ++i) {
        bluetooth_app__print_address(devices[i].address);
        stdio__printf(
            " rssi=%d name=%s\n", devices[i].rssi, devices[i].name[0] != '\0' ? devices[i].name : "(unnamed)"
        );
    }
    return 0;
}

static int bluetooth_app__scan_gui(void) {
    bluetooth__device_t devices[BLUETOOTH_APP__MAX_RESULTS];
    int count = bluetooth__scan_ble(devices, BLUETOOTH_APP__MAX_RESULTS, 5000);
    if (count < 0) {
        char message[48];
        snprintf(message, sizeof(message), "BLE scan failed (%d)", count);
        (void)bluetooth_app__message(BRUCE_DIALOG_ERROR, "Bluetooth", message);
        return count;
    }
    if (count == 0) {
        (void)bluetooth_app__message(BRUCE_DIALOG_INFO, "BLE Scan", "No advertisements found");
        return 0;
    }

    char labels[BLUETOOTH_APP__MAX_RESULTS][96];
    bruce_dialog_choice_t choices[BLUETOOTH_APP__MAX_RESULTS];
    for (int i = 0; i < count; ++i) {
        snprintf(
            labels[i],
            sizeof(labels[i]),
            "%s  %d dBm",
            devices[i].name[0] != '\0' ? devices[i].name : "(unnamed)",
            devices[i].rssi
        );
        choices[i].label = labels[i];
        choices[i].value = labels[i];
    }
    size_t selected = 0;
    bruce_result_t choice_result;
    do {
        choice_result = dialog__choice_launcher(
            "BLE Advertisements", "Select a device", choices, (size_t)count, &selected
        );
    } while (choice_result == BRUCE_ERR_CANCELLED && bluetooth_app__resume_after_handoff());
    if (choice_result == BRUCE_OK) {
        char details[160];
        bluetooth__device_t *device = &devices[selected];
        snprintf(
            details,
            sizeof(details),
            "Name: %s\nAddress: %02X:%02X:%02X:%02X:%02X:%02X\nRSSI: %d dBm",
            device->name[0] != '\0' ? device->name : "(unnamed)",
            device->address[0],
            device->address[1],
            device->address[2],
            device->address[3],
            device->address[4],
            device->address[5],
            device->rssi
        );
        (void)bluetooth_app__message(BRUCE_DIALOG_INFO, "BLE Device", details);
    }
    return 0;
}

int bluetooth_app_main(int argc, char **argv) {
    if (runtime__gui_requested()) {
        const bruce_dialog_choice_t choices[] = {
            {"BLE advertisement scan", "scan"}
        };
        size_t selected = 0;
        bruce_result_t choice_result;
        do {
            choice_result = dialog__choice_launcher("Bluetooth", "Select an action", choices, 1, &selected);
        } while (choice_result == BRUCE_ERR_CANCELLED && bluetooth_app__resume_after_handoff());
        if (choice_result == BRUCE_OK) { return bluetooth_app__scan_gui(); }
        return 0;
    }

    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Scan for nearby Bluetooth Low Energy advertisements.");
    ArgParser *scan = ap_new_cmd(root, "scan");
    ap_set_helptext(scan, "List nearby BLE advertisements.");
    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION)
            ap_print_help(ap_get_cmd_parser(root) != NULL ? ap_get_cmd_parser(root) : root);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }

    int result = bluetooth_app__scan_terminal();
    ap_free(root);
    return result;
}
