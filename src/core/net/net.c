#include "core_sdk/net.h"

#include <stdio.h>

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
