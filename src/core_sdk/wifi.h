#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Wi-Fi connections, scanning, and status.
 */

#define BRUCE_WIFI_SSID_MAX_LEN 32

typedef struct {
    char ssid[BRUCE_WIFI_SSID_MAX_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} wifi__network_t;

/**
 * @brief Disconnects from the current Wi-Fi network, if any.
 */
bruce_result_t wifi__disconnect(void);

/**
 * @brief Connects to a Wi-Fi network by SSID/password.
 *
 * @param ssid Network SSID to connect to.
 * @param password Network password, or NULL/"" for an open network.
 * @param timeout_ms Connection timeout in milliseconds.
 */
bruce_result_t wifi__connect(const char *ssid, const char *password, uint32_t timeout_ms);

/** @brief Connects using a previously saved Wi-Fi credential (see config.h). */
bruce_result_t wifi__connect_known(void);

/** @brief Starts the configured Wi-Fi access point. */
bruce_result_t wifi__setup_ap(void);

/**
 * @brief Scans for nearby Wi-Fi networks.
 *
 * Returns the number of networks copied into `networks` (0 on an empty
 * scan) or a negative BRUCE_ERR_* value on failure. Blocks the calling
 * process until the scan completes or times out; a caller that wants to
 * animate progress or offer cancellation should use wifi__scan_start() /
 * wifi__scan_poll() / wifi__scan_cancel() instead.
 *
 * @param networks Array to receive scanned networks.
 * @param capacity Number of entries networks can hold.
 */
int wifi__scan(wifi__network_t *networks, size_t capacity);

/**
 * @brief Starts a Wi-Fi scan without waiting for it to finish.
 *
 * Only one scan runs on the radio at a time; calling this while another
 * caller's scan is in flight (or uncollected) joins that round instead of
 * restarting it, so concurrent scanners share one result set rather than
 * stepping on each other. Must be matched by one wifi__scan_poll() call
 * that doesn't return BRUCE_ERR_TIMEOUT, or one wifi__scan_cancel().
 */
bruce_result_t wifi__scan_start(void);

/**
 * @brief Waits up to `timeout_ms` for a scan started with wifi__scan_start()
 * to finish, returning its results if it has.
 *
 * Returns the number of networks copied into `networks` (0 on an empty
 * scan), BRUCE_ERR_TIMEOUT if the scan is still running when `timeout_ms`
 * elapses (the scan itself is unaffected -- call again to keep waiting, or
 * wifi__scan_cancel() to give up on it), or another negative BRUCE_ERR_*
 * value on failure. Calling this without a scan in flight (no prior
 * wifi__scan_start(), or one already collected) just waits out the timeout
 * and returns BRUCE_ERR_TIMEOUT.
 *
 * A caller that joined the same round as another process still gets its
 * own copy, capped to its own `capacity`.
 *
 * @param networks Array to receive scanned networks.
 * @param capacity Number of entries networks can hold.
 * @param timeout_ms How long to wait for this poll before giving up.
 */
int wifi__scan_poll(wifi__network_t *networks, size_t capacity, uint32_t timeout_ms);

/**
 * @brief Aborts a scan started with wifi__scan_start(), if one is running.
 *
 * Best-effort and safe to call with no scan in flight.
 */
bruce_result_t wifi__scan_cancel(void);

/** @brief Whether Wi-Fi is currently connected to a network. Returns false when unavailable or denied. */
bool wifi__is_connected(void);

/** @brief Whether the Wi-Fi access point is currently running. Returns false when unavailable or denied. */
bool wifi__is_ap_running(void);

/**
 * @brief Currently connected SSID.
 *
 * Returns a pointer into Core-owned storage, or NULL when not currently
 * available; callers must not free it and should copy it before making
 * another wifi__* call.
 */
const char *wifi__get_ssid(void);

/**
 * @brief Currently assigned IP address.
 *
 * Returns a pointer into Core-owned storage, or NULL when not currently
 * available; callers must not free it and should copy it before making
 * another wifi__* call.
 */
const char *wifi__get_ip(void);

/**
 * @brief Wi-Fi station MAC address.
 *
 * Returns a pointer into Core-owned storage, or NULL when not currently
 * available; callers must not free it and should copy it before making
 * another wifi__* call.
 */
const char *wifi__get_mac(void);
