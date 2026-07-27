#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/bluetooth.h"
#include "core_sdk/result.h"

typedef enum {
    BRUCE_BLUETOOTH_HID_UNKNOWN = 0,
    BRUCE_BLUETOOTH_HID_KEYBOARD,
    BRUCE_BLUETOOTH_HID_GAMEPAD,
} bluetooth_hid__usage_t;

typedef struct {
    uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN];
    int8_t rssi;
    bluetooth_hid__usage_t usage;
    char name[BRUCE_BLUETOOTH_NAME_MAX + 1];
} bluetooth_hid__device_t;

/* Classic Bluetooth HID is available only on targets with a Classic-capable
 * radio. Unsupported targets return BRUCE_ERR_UNSUPPORTED. Scan returns a
 * count or a negative BRUCE_ERR_* value. */
bool bluetooth_hid__is_supported(void);
int bluetooth_hid__scan(bluetooth_hid__device_t *devices, size_t capacity, uint32_t timeout_ms);
bruce_result_t
bluetooth_hid__connect(const uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN], uint32_t timeout_ms);
bruce_result_t bluetooth_hid__disconnect(void);
bool bluetooth_hid__is_connected(void);
/* Status is permission-free; device identity requires `bt`. */
bruce_result_t bluetooth_hid__connected_device(bluetooth_hid__device_t *out_device);
