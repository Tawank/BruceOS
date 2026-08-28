#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Wi-Fi connect/scan/status for the existing Core Wi-Fi implementation.
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
 * scan) or a negative BRUCE_ERR_* value on failure.
 *
 * @param networks Array to receive scanned networks.
 * @param capacity Number of entries networks can hold.
 */
int wifi__scan(wifi__network_t *networks, size_t capacity);

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
