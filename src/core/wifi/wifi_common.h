#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/config/config.h"

typedef struct {
    char ssid[CONFIG__WIFI_SSID_MAX_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} wifi__network_t;

/* The core owns the ESP-IDF Wi-Fi driver; calls are safe from multiple tasks. */
bool wifi__init(void);
void wifi__disconnect(void);
bool wifi__connect(const char *ssid, const char *password, uint32_t timeout_ms);
bool wifi__connect_known(void);
bool wifi__setup_ap(void);
int wifi__scan(wifi__network_t *networks, size_t capacity);
bool wifi__is_connected(void);
bool wifi__is_ap_running(void);
bool wifi__get_ssid(char *ssid, size_t size);
bool wifi__get_ip(char *ip, size_t size);
bool wifi__get_mac(char *mac, size_t size);
bool wifi__add_credential(const char *ssid, const char *password);

/* Compatibility commands while their app-level implementations are ported. */
bool wifi__scan_hosts(void);
bool wifi__start_sniffer(void);
bool wifi__listen_tcp(void);
