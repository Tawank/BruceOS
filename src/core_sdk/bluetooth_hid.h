#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/bluetooth.h"
#include "core_sdk/result.h"

/**
 * @brief Bluetooth keyboard and gamepad connections.
 */

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

/**
 * @brief Whether Classic Bluetooth HID is available on this target.
 *
 * Only targets with a Classic-capable radio support it.
 */
bool bluetooth_hid__is_supported(void);

/**
 * @brief Scans for nearby Classic Bluetooth HID devices.
 *
 * Returns the number of devices copied to `devices`, or a negative
 * BRUCE_ERR_* value (including BRUCE_ERR_UNSUPPORTED on targets without a
 * Classic-capable radio).
 *
 * @param devices Array to receive scanned devices.
 * @param capacity Number of entries devices can hold.
 * @param timeout_ms Scan duration in milliseconds.
 */
int bluetooth_hid__scan(bluetooth_hid__device_t *devices, size_t capacity, uint32_t timeout_ms);

/**
 * @brief Connects to a Classic Bluetooth HID device.
 *
 * @param address Bluetooth address of the device to connect to.
 * @param timeout_ms Connection timeout in milliseconds.
 */
bruce_result_t
bluetooth_hid__connect(const uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN], uint32_t timeout_ms);

/**
 * @brief Disconnects the currently connected Classic Bluetooth HID device, if any.
 */
bruce_result_t bluetooth_hid__disconnect(void);

/**
 * @brief Whether a Classic Bluetooth HID device is currently connected.
 */
bool bluetooth_hid__is_connected(void);

/**
 * @brief Returns the currently connected HID device's identity.
 *
 * @param out_device Receives the connected device's identity.
 * @permission bt
 */
bruce_result_t bluetooth_hid__connected_device(bluetooth_hid__device_t *out_device);
