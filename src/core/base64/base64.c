#include "core_sdk/base64.h"

#include <stdbool.h>

static const char base64__alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bruce_result_t base64__encode(const void *data, size_t size, char *out, size_t out_capacity) {
    if (out == NULL || (data == NULL && size > 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    size_t encoded_size = BRUCE_BASE64_ENCODED_SIZE(size);
    if (out_capacity < encoded_size + 1) return BRUCE_ERR_RESOURCE_LIMIT;

    const uint8_t *bytes = (const uint8_t *)data;
    size_t written = 0;
    size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        uint32_t value = ((uint32_t)bytes[i] << 16) | ((uint32_t)bytes[i + 1] << 8) | bytes[i + 2];
        out[written++] = base64__alphabet[(value >> 18) & 0x3Fu];
        out[written++] = base64__alphabet[(value >> 12) & 0x3Fu];
        out[written++] = base64__alphabet[(value >> 6) & 0x3Fu];
        out[written++] = base64__alphabet[value & 0x3Fu];
    }
    size_t remaining = size - i;
    if (remaining == 1) {
        uint32_t value = (uint32_t)bytes[i] << 16;
        out[written++] = base64__alphabet[(value >> 18) & 0x3Fu];
        out[written++] = base64__alphabet[(value >> 12) & 0x3Fu];
        out[written++] = '=';
        out[written++] = '=';
    } else if (remaining == 2) {
        uint32_t value = ((uint32_t)bytes[i] << 16) | ((uint32_t)bytes[i + 1] << 8);
        out[written++] = base64__alphabet[(value >> 18) & 0x3Fu];
        out[written++] = base64__alphabet[(value >> 12) & 0x3Fu];
        out[written++] = base64__alphabet[(value >> 6) & 0x3Fu];
        out[written++] = '=';
    }
    out[written] = '\0';
    return BRUCE_OK;
}

/* -1 for anything outside the alphabet, including '=' -- padding is
 * recognized separately by literal comparison in base64__decode(), never
 * by this lookup. */
static int base64__value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64__is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bruce_result_t
base64__decode(const char *text, size_t text_length, uint8_t *out, size_t out_capacity, size_t *out_size) {
    if (out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if ((text == NULL && text_length > 0) || (out == NULL && out_capacity > 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (out_capacity < BRUCE_BASE64_DECODED_SIZE(text_length)) return BRUCE_ERR_RESOURCE_LIMIT;

    char group[4];
    size_t group_len = 0;
    size_t written = 0;
    bool padding_seen = false;
    for (size_t i = 0; i <= text_length; ++i) {
        bool end = i == text_length;
        if (!end) {
            char c = text[i];
            if (base64__is_space(c)) continue;
            /* Once padding starts, nothing but more input beyond it is
             * allowed -- this both rejects '=' in the middle of a group
             * and, since it fires on every following character, guarantees
             * padding can only ever be part of the very last group. */
            if (padding_seen) return BRUCE_ERR_INVALID_ARGUMENT;
            if (c == '=') {
                padding_seen = true;
            } else if (base64__value(c) < 0) {
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
            group[group_len++] = c;
        }
        if (group_len == 4 || (end && group_len > 0)) {
            if (end && group_len != 4) return BRUCE_ERR_INVALID_ARGUMENT;
            int v0 = base64__value(group[0]);
            int v1 = base64__value(group[1]);
            bool pad2 = group[2] == '=';
            bool pad3 = group[3] == '=';
            if (v0 < 0 || v1 < 0 || (pad2 && !pad3)) return BRUCE_ERR_INVALID_ARGUMENT;
            int v2 = pad2 ? 0 : base64__value(group[2]);
            int v3 = pad3 ? 0 : base64__value(group[3]);
            if (v2 < 0 || v3 < 0) return BRUCE_ERR_INVALID_ARGUMENT;
            out[written++] = (uint8_t)((v0 << 2) | (v1 >> 4));
            if (!pad2) out[written++] = (uint8_t)((v1 << 4) | (v2 >> 2));
            if (!pad3) out[written++] = (uint8_t)((v2 << 6) | v3);
            group_len = 0;
        }
    }
    *out_size = written;
    return BRUCE_OK;
}
