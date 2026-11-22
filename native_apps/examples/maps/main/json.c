#include "json.h"

#include <stdlib.h>
#include <string.h>

void json__skip_space(json_t *json) {
    while (json->p < json->end && (*json->p == ' ' || *json->p == '\n' || *json->p == '\r' || *json->p == '\t')) {
        json->p++;
    }
}

bool json__eat(json_t *json, char c) {
    json__skip_space(json);
    if (json->p >= json->end || *json->p != c) return false;
    json->p++;
    return true;
}

static int json__hex(json_t *json) {
    if (json->end - json->p < 4) return -1;
    int value = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *json->p++;
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return -1;
        value = value * 16 + digit;
    }
    return value;
}

/* Appends whole UTF-8 sequences only, so truncation never splits a character. */
static void json__put(char *out, size_t capacity, size_t *len, const char *bytes, size_t count) {
    if (out == NULL || *len + count >= capacity) return;
    memcpy(out + *len, bytes, count);
    *len += count;
    out[*len] = '\0';
}

static void json__put_code_point(char *out, size_t capacity, size_t *len, int cp) {
    char bytes[3];
    if (cp < 0x80) {
        bytes[0] = (char)cp;
        json__put(out, capacity, len, bytes, 1);
    } else if (cp < 0x800) {
        bytes[0] = (char)(0xC0 | cp >> 6);
        bytes[1] = (char)(0x80 | (cp & 0x3F));
        json__put(out, capacity, len, bytes, 2);
    } else {
        bytes[0] = (char)(0xE0 | cp >> 12);
        bytes[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        bytes[2] = (char)(0x80 | (cp & 0x3F));
        json__put(out, capacity, len, bytes, 3);
    }
}

static size_t json__utf8_length(unsigned char lead) {
    return lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
}

/* Reads a string into `out` (truncated), or skips it when `out` is NULL. */
bool json__string(json_t *json, char *out, size_t capacity) {
    size_t len = 0;
    if (out != NULL && capacity > 0) out[0] = '\0';
    if (!json__eat(json, '"')) return false;
    while (json->p < json->end) {
        char c = *json->p++;
        if (c == '"') return true;
        if (c != '\\') {
            size_t count = json__utf8_length((unsigned char)c);
            if ((size_t)(json->end - json->p) < count - 1) return false;
            json__put(out, capacity, &len, json->p - 1, count);
            json->p += count - 1;
            continue;
        }
        if (json->p >= json->end) return false;
        char escaped = *json->p++;
        static const char from[] = "\"\\/bfnrt", to[] = "\"\\/\b\f\n\r\t";
        const char *known = strchr(from, escaped);
        if (escaped == 'u') {
            int cp = json__hex(json);
            if (cp < 0) return false;
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?'; /* Emoji and other surrogate pairs. */
            json__put_code_point(out, capacity, &len, cp);
        } else if (known != NULL && escaped != '\0') {
            json__put(out, capacity, &len, &to[known - from], 1);
        } else {
            return false;
        }
    }
    return false;
}

bool json__skip_value(json_t *json) {
    json__skip_space(json);
    if (json->p >= json->end) return false;
    if (*json->p == '"') return json__string(json, NULL, 0);
    if (*json->p == '{' || *json->p == '[') {
        int depth = 0;
        do {
            if (json->p >= json->end) return false;
            if (*json->p == '"') {
                if (!json__string(json, NULL, 0)) return false;
                continue;
            }
            if (*json->p == '{' || *json->p == '[') depth++;
            else if (*json->p == '}' || *json->p == ']') depth--;
            json->p++;
        } while (depth > 0);
        return true;
    }
    while (json->p < json->end && strchr(",}] \n\r\t", *json->p) == NULL) json->p++;
    return true;
}

/* Nominatim sends coordinates as strings; numbers are accepted too. */
bool json__number(json_t *json, double *out) {
    char text[32];
    json__skip_space(json);
    if (json->p < json->end && *json->p == '"') {
        if (!json__string(json, text, sizeof(text))) return false;
    } else {
        const char *start = json->p;
        if (!json__skip_value(json) || (size_t)(json->p - start) >= sizeof(text)) return false;
        memcpy(text, start, (size_t)(json->p - start));
        text[json->p - start] = '\0';
    }
    char *end = NULL;
    *out = strtod(text, &end);
    return end != text;
}

