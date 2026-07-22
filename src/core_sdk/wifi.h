#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_WIFI_SSID_MAX_LEN 32

typedef struct {
    char ssid[BRUCE_WIFI_SSID_MAX_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} wifi__network_t;

/* Public declarations for the existing Core Wi-Fi implementation in
 * wifi_common.c.  Not every function here returns bruce_result_t: simple
 * getters use the type that is easiest for a caller to use directly, and
 * document their own failure value instead of forcing an output parameter.
 * wifi__scan returns the number of networks copied into `networks` (0 on an
 * empty scan) or a negative BRUCE_ERR_* value on failure.  wifi__is_connected
 * and wifi__is_ap_running cannot fail, so they return bool directly.
 * wifi__get_ssid/get_ip/get_mac return a pointer into Core-owned storage, or
 * NULL when the value is not currently available; callers must not free it
 * and should copy it before making another wifi__* call.  A8 adds permission
 * enforcement in this implementation without a duplicate façade. */
bruce_result_t wifi__disconnect(void);
bruce_result_t wifi__connect(const char *ssid, const char *password, uint32_t timeout_ms);
bruce_result_t wifi__connect_known(void);
bruce_result_t wifi__setup_ap(void);
int wifi__scan(wifi__network_t *networks, size_t capacity);
bool wifi__is_connected(void);
bool wifi__is_ap_running(void);
char *wifi__get_ssid(void);
char *wifi__get_ip(void);
char *wifi__get_mac(void);
bruce_result_t wifi__add_credential(const char *ssid, const char *password);
bruce_result_t wifi__scan_hosts(void);
bruce_result_t wifi__start_sniffer(void);
bruce_result_t wifi__listen_tcp(void);
