#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief BLE advertisement scanning.
 */

#define BRUCE_BLUETOOTH_NAME_MAX 63
#define BRUCE_BLUETOOTH_ADDRESS_LEN 6

typedef struct {
    uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN];
    uint8_t address_type;
    int8_t rssi;
    char name[BRUCE_BLUETOOTH_NAME_MAX + 1];
} bluetooth__device_t;

/**
 * @brief Performs a synchronous BLE advertisement scan.
 *
 * Returns the number of unique devices copied to `devices`, or a negative
 * BRUCE_ERR_* value. A zero timeout selects the Core default. Results are
 * ordered by descending RSSI. Blocks the calling process for the scan's
 * duration; a caller that wants to animate progress or offer cancellation
 * should use bluetooth__scan_start() / bluetooth__scan_poll() /
 * bluetooth__scan_cancel() instead.
 *
 * @param devices Array to receive scanned devices.
 * @param capacity Number of entries devices can hold.
 * @param timeout_ms Scan duration in milliseconds, or 0 for the Core default.
 */
int bluetooth__scan_ble(bluetooth__device_t *devices, size_t capacity, uint32_t timeout_ms);

/**
 * @brief Starts a BLE advertisement scan without waiting for it to finish.
 *
 * Only one scan runs on the radio at a time; calling this while another
 * caller's scan is in flight (or uncollected) joins that round instead of
 * restarting it, so concurrent scanners share one result set rather than
 * stepping on each other -- the joining caller's own `timeout_ms` is then
 * ignored, since the round is already running to the first caller's. Must
 * be matched by one bluetooth__scan_poll() call that doesn't return
 * BRUCE_ERR_TIMEOUT, or one bluetooth__scan_cancel().
 *
 * @param timeout_ms Scan duration in milliseconds, or 0 for the Core default.
 */
bruce_result_t bluetooth__scan_start(uint32_t timeout_ms);

/**
 * @brief Waits up to `timeout_ms` for a scan started with
 * bluetooth__scan_start() to finish, returning its results if it has.
 *
 * Returns the number of devices copied into `devices` (0 on an empty scan),
 * BRUCE_ERR_TIMEOUT if the scan is still running when `timeout_ms` elapses
 * (the scan itself is unaffected -- call again to keep waiting, or
 * bluetooth__scan_cancel() to give up on it), or another negative
 * BRUCE_ERR_* value on failure. Calling this without a scan in flight (no
 * prior bluetooth__scan_start(), or one already collected) just waits out
 * the timeout and returns BRUCE_ERR_TIMEOUT. Results are ordered by
 * descending RSSI.
 *
 * A caller that joined the same round as another process still gets its
 * own copy, capped to its own `capacity`.
 *
 * @param devices Array to receive scanned devices.
 * @param capacity Number of entries devices can hold.
 * @param timeout_ms How long to wait for this poll before giving up.
 */
int bluetooth__scan_poll(bluetooth__device_t *devices, size_t capacity, uint32_t timeout_ms);

/**
 * @brief Aborts a scan started with bluetooth__scan_start(), if one is running.
 *
 * Best-effort and safe to call with no scan in flight.
 */
bruce_result_t bluetooth__scan_cancel(void);
