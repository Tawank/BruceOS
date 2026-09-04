#include "bluetooth_app.h"

#include <stdio.h>

#include "args.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/dialog.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

#define BLUETOOTH_APP__MAX_RESULTS 32
/* Scan window passed to bluetooth__scan_start()/bluetooth__scan_ble(). */
#define BLUETOOTH_APP__SCAN_MS 5000u
#define BLUETOOTH_APP__SCAN_POLL_INTERVAL_MS 100u

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
    int count = bluetooth__scan_ble(devices, BLUETOOTH_APP__MAX_RESULTS, BLUETOOTH_APP__SCAN_MS);
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

typedef struct {
    bluetooth__device_t *devices;
    size_t capacity;
    int result;
    bool active;
} bluetooth_app_gui_scan_t;

static bruce_result_t bluetooth_app__poll_scan(void *context, bool *out_complete) {
    bluetooth_app_gui_scan_t *scan = context;
    scan->result = bluetooth__scan_poll(scan->devices, scan->capacity, 0);
    if (scan->result == BRUCE_ERR_TIMEOUT) { return BRUCE_OK; }
    scan->active = false;
    *out_complete = true;
    return BRUCE_OK;
}

static void bluetooth_app__cancel_scan(void *context) {
    bluetooth_app_gui_scan_t *scan = context;
    if (scan->active) (void)bluetooth__scan_cancel();
}

/* Presents the scan as a live choice dialog instead of blocking on one long
 * scan wait -- same shape as wifi_app__gui_scan() (wifi_app.c). Back leaves
 * through dialog cleanup, which aborts only a scan that has not already
 * completed. */
static int bluetooth_app__gui_scan(bluetooth__device_t *devices, size_t capacity) {
    bruce_result_t start = bluetooth__scan_start(BLUETOOTH_APP__SCAN_MS);
    if (start != BRUCE_OK) return (int)start;
    bluetooth_app_gui_scan_t scan = {
        .devices = devices, .capacity = capacity, .result = BRUCE_ERR_TIMEOUT, .active = true
    };
    const bruce_dialog_choice_t choices[] = {
        {.label = "Back", .value = "back"},
    };
    size_t selected = 0;
    bool complete = false;
    bruce_result_t dialog_result = dialog__choice_poll_launcher(
        "BLE Scanning...", NULL, choices, sizeof(choices) / sizeof(choices[0]),
        BLUETOOTH_APP__SCAN_POLL_INTERVAL_MS, bluetooth_app__poll_scan, &scan, bluetooth_app__cancel_scan, &selected,
        &complete
    );
    if (dialog_result == BRUCE_ERR_CANCELLED || (dialog_result == BRUCE_OK && !complete)) return BRUCE_ERR_CANCELLED;
    if (dialog_result != BRUCE_OK) return (int)dialog_result;
    return scan.result;
}

/* devices/labels/choices are heap-allocated (rather than kept as ~6 KB of
 * combined locals) so this fits comfortably in the app's small process
 * stack; see memory__malloc()'s process-scoped allocator in core_sdk/memory.h. */
static int bluetooth_app__scan_gui(void) {
    bluetooth__device_t *devices = memory__calloc(BLUETOOTH_APP__MAX_RESULTS, sizeof(*devices));
    if (devices == NULL) return BRUCE_ERR_NO_MEMORY;

    for (;;) {
        int count = bluetooth_app__gui_scan(devices, BLUETOOTH_APP__MAX_RESULTS);
        if (count == BRUCE_ERR_CANCELLED) break;
        if (count < 0) {
            char message[48];
            snprintf(message, sizeof(message), "BLE scan failed (%d)", count);
            (void)bluetooth_app__message(BRUCE_DIALOG_ERROR, "Bluetooth", message);
            memory__free(devices);
            return count;
        }
        if (count == 0) {
            const bruce_dialog_choice_t choices[] = {
                {.label = "Rescan", .value = "rescan", .icon_name = "bluetooth"},
                {.label = "Back", .value = "back"},
            };
            size_t selected = 0;
            bruce_result_t choice_result;
            do {
                choice_result = dialog__choice_launcher(
                    "BLE Scan", "No advertisements found", choices, sizeof(choices) / sizeof(choices[0]), &selected
                );
            } while (choice_result == BRUCE_ERR_CANCELLED && bluetooth_app__resume_after_handoff());
            if (choice_result == BRUCE_OK && selected == 0) continue;
            break;
        }

        char (*labels)[96] = memory__calloc((size_t)count, sizeof(*labels));
        bruce_dialog_choice_t *choices = memory__calloc((size_t)count + 2u, sizeof(*choices));
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
        }
        choices[count] = (bruce_dialog_choice_t){.label = "Rescan", .value = "rescan"};
        choices[count + 1] = (bruce_dialog_choice_t){.label = "Back", .value = "back"};

        bool rescan = false;
        for (;;) {
            size_t selected = 0;
            bruce_result_t choice_result;
            do {
                choice_result = dialog__choice_launcher(
                    "BLE Advertisements", NULL, choices, (size_t)count + 2u, &selected
                );
            } while (choice_result == BRUCE_ERR_CANCELLED && bluetooth_app__resume_after_handoff());
            if (choice_result != BRUCE_OK || selected == (size_t)count + 1u) break;
            if (selected == (size_t)count) {
                rescan = true;
                break;
            }
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
        if (!rescan) break;
    }
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
