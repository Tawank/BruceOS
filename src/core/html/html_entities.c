#include <stdint.h>
#include <string.h>

#include "html_internal.h"

/* A small table of the named entities that actually show up in ordinary web
 * page text; this is not the full HTML5 named-character-reference table
 * (which has thousands of entries most pages never use). Anything else falls
 * back to numeric references or is left as literal text by the caller. */
typedef struct {
    const char *name;
    const char *value;
} html_named_entity_t;

static const html_named_entity_t HTML_NAMED_ENTITIES[] = {
    {"amp",     "&"    },
    {"lt",      "<"    },
    {"gt",      ">"    },
    {"quot",    "\""   },
    {"apos",    "'"    },
    {"nbsp",    " "    },
    {"copy",    "\xc2\xa9"},
    {"reg",     "\xc2\xae"},
    {"trade",   "\xe2\x84\xa2"},
    {"mdash",   "\xe2\x80\x94"},
    {"ndash",   "\xe2\x80\x93"},
    {"hellip",  "\xe2\x80\xa6"},
    {"lsquo",   "\xe2\x80\x98"},
    {"rsquo",   "\xe2\x80\x99"},
    {"ldquo",   "\xe2\x80\x9c"},
    {"rdquo",   "\xe2\x80\x9d"},
    {"middot",  "\xc2\xb7"},
    {"bull",    "\xe2\x80\xa2"},
    {"deg",     "\xc2\xb0"},
};

/* Encodes one Unicode code point as UTF-8 into `out` (up to 4 bytes). Returns
 * the number of bytes written, or 0 if it doesn't fit `out_capacity`. */
static size_t html__utf8_encode(uint32_t code_point, char *out, size_t out_capacity) {
    if (code_point <= 0x7F) {
        if (out_capacity < 1) return 0;
        out[0] = (char)code_point;
        return 1;
    }
    if (code_point <= 0x7FF) {
        if (out_capacity < 2) return 0;
        out[0] = (char)(0xC0 | (code_point >> 6));
        out[1] = (char)(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point <= 0xFFFF) {
        if (out_capacity < 3) return 0;
        out[0] = (char)(0xE0 | (code_point >> 12));
        out[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code_point & 0x3F));
        return 3;
    }
    if (code_point <= 0x10FFFF) {
        if (out_capacity < 4) return 0;
        out[0] = (char)(0xF0 | (code_point >> 18));
        out[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code_point & 0x3F));
        return 4;
    }
    return 0;
}

static bool html__decode_numeric_entity(const char *name, size_t name_len, char *out, size_t out_capacity, size_t *out_len) {
    if (name_len < 2 || name[0] != '#') return false;
    size_t i = 1;
    uint32_t base = 10;
    if (name[i] == 'x' || name[i] == 'X') {
        base = 16;
        i++;
    }
    if (i >= name_len) return false;
    uint32_t code_point = 0;
    for (; i < name_len; ++i) {
        char c = name[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else return false;
        if (digit >= base) return false;
        code_point = code_point * base + digit;
        if (code_point > 0x10FFFF) return false;
    }
    size_t written = html__utf8_encode(code_point, out, out_capacity);
    if (written == 0) return false;
    *out_len = written;
    return true;
}

bool html__decode_entity(const char *name, size_t name_len, char *out, size_t out_capacity, size_t *out_len) {
    if (name == NULL || name_len == 0 || out == NULL || out_len == NULL) return false;
    if (name[0] == '#') return html__decode_numeric_entity(name, name_len, out, out_capacity, out_len);

    for (size_t i = 0; i < sizeof(HTML_NAMED_ENTITIES) / sizeof(HTML_NAMED_ENTITIES[0]); ++i) {
        const char *candidate = HTML_NAMED_ENTITIES[i].name;
        if (strlen(candidate) == name_len && memcmp(candidate, name, name_len) == 0) {
            size_t value_len = strlen(HTML_NAMED_ENTITIES[i].value);
            if (value_len > out_capacity) return false;
            memcpy(out, HTML_NAMED_ENTITIES[i].value, value_len);
            *out_len = value_len;
            return true;
        }
    }
    return false;
}
