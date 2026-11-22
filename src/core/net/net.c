#include "core_sdk/net.h"

#include <stdio.h>

#include "core/cellular/cellular.h"
#include "core/wifi/wifi_common.h"
#include "esp_timer.h"

#define NET__MOBILE_WAIT_US (60 * 1000 * 1000LL)

static volatile int64_t s_connect_started_us;

bool net__is_online(void) { return wifi__is_connected_internal() || cellular__data_online(); }

bruce_result_t net__connect_start(void) {
    if (net__is_online()) return BRUCE_OK;
    s_connect_started_us = esp_timer_get_time();
    bruce_result_t result = wifi__connect_known_start_internal();
    /* Without Wi-Fi, mobile data may still come. */
    return result == BRUCE_OK || cellular__data_pending() ? BRUCE_OK : result;
}

bruce_result_t net__connect_poll(void) {
    if (net__is_online()) return BRUCE_OK;
    bruce_result_t wifi = wifi__connect_known_poll_internal();
    if (wifi == BRUCE_ERR_BUSY) return BRUCE_ERR_BUSY;
    if (!cellular__data_pending()) return wifi == BRUCE_OK ? BRUCE_ERR_IO : wifi;
    return esp_timer_get_time() - s_connect_started_us < NET__MOBILE_WAIT_US ? BRUCE_ERR_BUSY : BRUCE_ERR_TIMEOUT;
}

bruce_result_t net__parse_ipv4(const char *text, uint32_t *out_address) {
    if (text == NULL || out_address == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    unsigned int octets[4];
    int consumed = 0;
    if (sscanf(text, "%3u.%3u.%3u.%3u%n", &octets[0], &octets[1], &octets[2], &octets[3], &consumed) != 4) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (text[consumed] != '\0') return BRUCE_ERR_INVALID_ARGUMENT; /* trailing garbage, e.g. a hostname */
    for (int i = 0; i < 4; ++i) {
        if (octets[i] > 255) return BRUCE_ERR_INVALID_ARGUMENT;
    }

    *out_address =
        ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) | ((uint32_t)octets[2] << 8) | (uint32_t)octets[3];
    return BRUCE_OK;
}

void net__format_ipv4(uint32_t address, char *out, size_t capacity) {
    if (out == NULL || capacity == 0) return;
    snprintf(
        out, capacity, "%u.%u.%u.%u", (unsigned int)((address >> 24) & 0xFFu),
        (unsigned int)((address >> 16) & 0xFFu), (unsigned int)((address >> 8) & 0xFFu),
        (unsigned int)(address & 0xFFu)
    );
}
