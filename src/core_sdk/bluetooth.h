#pragma once

#include <stddef.h>
#include <stdint.h>

#define BRUCE_BLUETOOTH_NAME_MAX 63
#define BRUCE_BLUETOOTH_ADDRESS_LEN 6

typedef struct {
    uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN];
    uint8_t address_type;
    int8_t rssi;
    char name[BRUCE_BLUETOOTH_NAME_MAX + 1];
} bluetooth__device_t;

/* Performs a synchronous BLE advertisement scan. Returns the number of unique
 * devices copied to `devices`, or a negative BRUCE_ERR_* value. A zero timeout
 * selects the Core default. Results are ordered by descending RSSI. */
int bluetooth__scan_ble(bluetooth__device_t *devices, size_t capacity, uint32_t timeout_ms);
