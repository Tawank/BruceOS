#include "bluetooth_app.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/dialog.h"

#define BLUETOOTH_APP__MAX_RESULTS 32

static void bluetooth_app__print_address(const uint8_t *address)
{
    printf("%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2],
           address[3], address[4], address[5]);
}

static int bluetooth_app__scan_terminal(void)
{
    bluetooth__device_t devices[BLUETOOTH_APP__MAX_RESULTS];
    int count = bluetooth__scan_ble(devices, BLUETOOTH_APP__MAX_RESULTS, 5000);
    if (count < 0) {
        printf("BLE scan failed: %d\n", count);
        return count;
    }
    for (int i = 0; i < count; ++i) {
        bluetooth_app__print_address(devices[i].address);
        printf(" rssi=%d name=%s\n", devices[i].rssi,
               devices[i].name[0] != '\0' ? devices[i].name : "(unnamed)");
    }
    return 0;
}

static int bluetooth_app__scan_gui(void)
{
    bluetooth__device_t devices[BLUETOOTH_APP__MAX_RESULTS];
    int count = bluetooth__scan_ble(devices, BLUETOOTH_APP__MAX_RESULTS, 5000);
    if (count < 0) {
        char message[48];
        snprintf(message, sizeof(message), "BLE scan failed (%d)", count);
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Bluetooth", message);
        return count;
    }
    if (count == 0) {
        (void)dialog__message(BRUCE_DIALOG_INFO, "BLE Scan", "No advertisements found");
        return 0;
    }

    char labels[BLUETOOTH_APP__MAX_RESULTS][96];
    bruce_dialog_choice_t choices[BLUETOOTH_APP__MAX_RESULTS];
    for (int i = 0; i < count; ++i) {
        snprintf(labels[i], sizeof(labels[i]), "%s  %d dBm",
                 devices[i].name[0] != '\0' ? devices[i].name : "(unnamed)", devices[i].rssi);
        choices[i].label = labels[i];
        choices[i].value = labels[i];
    }
    size_t selected = 0;
    if (dialog__choice("BLE Advertisements", "Select a device", choices, (size_t)count, &selected, NULL) == BRUCE_OK) {
        char details[160];
        bluetooth__device_t *device = &devices[selected];
        snprintf(details, sizeof(details),
                 "Name: %s\nAddress: %02X:%02X:%02X:%02X:%02X:%02X\nRSSI: %d dBm",
                 device->name[0] != '\0' ? device->name : "(unnamed)", device->address[0], device->address[1],
                 device->address[2], device->address[3], device->address[4], device->address[5], device->rssi);
        (void)dialog__message(BRUCE_DIALOG_INFO, "BLE Device", details);
    }
    return 0;
}

int bluetooth_app_main(int argc, char **argv)
{
    if (app_runner__args_have_gui(argc, argv)) {
        const bruce_dialog_choice_t choices[] = {{"BLE advertisement scan", "scan"}};
        size_t selected = 0;
        if (dialog__choice("Bluetooth", "Select an action", choices, 1, &selected, NULL) == BRUCE_OK) {
            return bluetooth_app__scan_gui();
        }
        return 0;
    }
    if (argc == 0 || argv == NULL || argv[0] == NULL || strcmp(argv[0], "scan") == 0) {
        return bluetooth_app__scan_terminal();
    }
    printf("Usage: bluetooth scan\n");
    return BRUCE_ERR_INVALID_ARGUMENT;
}
