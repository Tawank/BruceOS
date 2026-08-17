#include "bluetooth_app.h"

#include <stdio.h>

#include "args.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define BLUETOOTH_APP__MAX_RESULTS 32

/* bluetooth__scan_ble() blocks for the whole scan window (default 5s); paint
 * a status screen first so the display isn't left showing the previous menu
 * (or nothing) for that whole stretch, matching image_viewer_app's
 * "Loading..." screen. Best-effort: scanning still proceeds even if a draw
 * call fails. */
static void bluetooth_app__show_scanning(void) {
    if (display__begin_frame() != BRUCE_OK) return;
    (void)display__fill_screen(BRUCE_COLOR_BLACK);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(BRUCE_COLOR_WHITE);
    (void)display__set_text_size(2);
    (void)display__draw_centre_string("Scanning...", display__width() / 2, (display__height() - 8) / 2);
    (void)display__present();
}

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

/* devices/labels/choices are heap-allocated (rather than kept as ~6 KB of
 * combined locals) so this fits comfortably in the app's small process
 * stack; see memory__malloc()'s process-scoped allocator in core_sdk/memory.h. */
static int bluetooth_app__scan_gui(void) {
    bluetooth__device_t *devices = memory__calloc(BLUETOOTH_APP__MAX_RESULTS, sizeof(*devices));
    if (devices == NULL) return BRUCE_ERR_NO_MEMORY;

    bluetooth_app__show_scanning();
    int count = bluetooth__scan_ble(devices, BLUETOOTH_APP__MAX_RESULTS, 5000);
    if (count < 0) {
        char message[48];
        snprintf(message, sizeof(message), "BLE scan failed (%d)", count);
        (void)bluetooth_app__message(BRUCE_DIALOG_ERROR, "Bluetooth", message);
        memory__free(devices);
        return count;
    }
    if (count == 0) {
        (void)bluetooth_app__message(BRUCE_DIALOG_INFO, "BLE Scan", "No advertisements found");
        memory__free(devices);
        return 0;
    }

    char (*labels)[96] = memory__calloc((size_t)count, sizeof(*labels));
    bruce_dialog_choice_t *choices = memory__calloc((size_t)count, sizeof(*choices));
    if (labels == NULL || choices == NULL) {
        memory__free(labels);
        memory__free(choices);
        memory__free(devices);
        return BRUCE_ERR_NO_MEMORY;
    }
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
        choices[i].icon_name = "bluetooth";
        choices[i].right_text = NULL;
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
    memory__free(labels);
    memory__free(choices);
    memory__free(devices);
    return 0;
}

/* The launcher's Bluetooth submenu (see embedded_resources/json/launcher.json)
 * declares its one action as "bluetooth scan", the same way WiFi's submenu
 * declares "wifi scan" etc. -- so, like wifi_app_main(), this dispatches on
 * the parsed subcommand in both GUI and terminal mode instead of showing its
 * own hardcoded chooser menu. Scan is also the default action, matching a
 * bare "bluetooth" invocation (e.g. from the Apps list) to the single item
 * the old internal chooser offered. */
int bluetooth_app_main(int argc, char **argv) {
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

    int result = runtime__gui_requested() ? bluetooth_app__scan_gui() : bluetooth_app__scan_terminal();
    ap_free(root);
    return result;
}
