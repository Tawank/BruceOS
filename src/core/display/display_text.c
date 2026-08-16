#include "display_internal.h"

#include "core_sdk/display.h"

static const uint8_t s_font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50},
    {0x00, 0x08, 0x07, 0x03, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x2A, 0x1C, 0x7F, 0x1C, 0x2A},
    {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46},
    {0x21, 0x41, 0x49, 0x4D, 0x33},
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x31},
    {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x46, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x00, 0x14, 0x00, 0x00},
    {0x00, 0x40, 0x34, 0x00, 0x00},
    {0x00, 0x08, 0x14, 0x22, 0x41},
    {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08},
    {0x02, 0x01, 0x59, 0x09, 0x06},
    {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    {0x7C, 0x12, 0x11, 0x12, 0x7C},
    {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x73},
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32},
    {0x03, 0x01, 0x7F, 0x01, 0x03},
    {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03},
    {0x61, 0x51, 0x49, 0x45, 0x43},
    {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20},
    {0x00, 0x41, 0x41, 0x41, 0x7F},
    {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x03, 0x07, 0x08, 0x00},
    {0x20, 0x54, 0x54, 0x78, 0x40},
    {0x7F, 0x28, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x28},
    {0x38, 0x44, 0x44, 0x28, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x00, 0x08, 0x7E, 0x09, 0x02},
    {0x18, 0xA4, 0xA4, 0x9C, 0x78},
    {0x7F, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x40, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00},
    {0x7C, 0x04, 0x78, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38},
    {0xFC, 0x18, 0x24, 0x24, 0x18},
    {0x18, 0x24, 0x24, 0x18, 0xFC},
    {0x7C, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3F, 0x44, 0x24},
    {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x4C, 0x90, 0x90, 0x90, 0x7C},
    {0x44, 0x64, 0x54, 0x4C, 0x44},
    {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x77, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00},
    {0x02, 0x01, 0x02, 0x04, 0x02},
};

static const uint8_t s_font_dotless_i[5] = {0x00, 0x44, 0x7C, 0x40, 0x00};

const uint8_t *display_internal__font_glyph(char c) {
    if (c < DISPLAY__FONT_FIRST || c > DISPLAY__FONT_LAST) { return NULL; }
    return s_font_5x7[(int)c - DISPLAY__FONT_FIRST];
}

typedef enum {
    DISPLAY__ACCENT_NONE,
    DISPLAY__ACCENT_ACUTE,
    DISPLAY__ACCENT_GRAVE,
    DISPLAY__ACCENT_CIRCUMFLEX,
    DISPLAY__ACCENT_TILDE,
    DISPLAY__ACCENT_DIAERESIS,
    DISPLAY__ACCENT_CEDILLA,
    DISPLAY__ACCENT_CARON,
    DISPLAY__ACCENT_BREVE,
    DISPLAY__ACCENT_RING,
    DISPLAY__ACCENT_STROKE,
    DISPLAY__ACCENT_DOUBLE_ACUTE,
    DISPLAY__ACCENT_OGONEK,
    DISPLAY__ACCENT_DOT_ABOVE,
    DISPLAY__ACCENT_APOSTROPHE,
} display__accent_t;

static size_t display__utf8_decode(const char *text, uint32_t *out_codepoint) {
    const uint8_t *p = (const uint8_t *)text;
    uint32_t codepoint = p[0];
    size_t length = 1;
    if (p[0] >= 0xC2 && p[0] <= 0xDF && (p[1] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        length = 2;
    } else if (p[0] >= 0xE0 && p[0] <= 0xEF && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        length = 3;
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 7) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                    ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        length = 4;
    }
    *out_codepoint = codepoint;
    return length;
}

static char display__latin_base(uint32_t c, display__accent_t *out_accent) {
    *out_accent = DISPLAY__ACCENT_NONE;
    switch (c) {
        case 0x00C0: case 0x00E0: *out_accent = DISPLAY__ACCENT_GRAVE; return c == 0x00C0 ? 'A' : 'a';
        case 0x00C1: case 0x00E1: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x00C1 ? 'A' : 'a';
        case 0x00C2: case 0x00E2: *out_accent = DISPLAY__ACCENT_CIRCUMFLEX; return c == 0x00C2 ? 'A' : 'a';
        case 0x00C3: case 0x00E3: *out_accent = DISPLAY__ACCENT_TILDE; return c == 0x00C3 ? 'A' : 'a';
        case 0x00C4: case 0x00E4: *out_accent = DISPLAY__ACCENT_DIAERESIS; return c == 0x00C4 ? 'A' : 'a';
        case 0x00C7: case 0x00E7: *out_accent = DISPLAY__ACCENT_CEDILLA; return c == 0x00C7 ? 'C' : 'c';
        case 0x00C8: case 0x00E8: *out_accent = DISPLAY__ACCENT_GRAVE; return c == 0x00C8 ? 'E' : 'e';
        case 0x00C9: case 0x00E9: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x00C9 ? 'E' : 'e';
        case 0x00CA: case 0x00EA: *out_accent = DISPLAY__ACCENT_CIRCUMFLEX; return c == 0x00CA ? 'E' : 'e';
        case 0x00CB: case 0x00EB: *out_accent = DISPLAY__ACCENT_DIAERESIS; return c == 0x00CB ? 'E' : 'e';
        case 0x00CC: case 0x00EC: *out_accent = DISPLAY__ACCENT_GRAVE; return c == 0x00CC ? 'I' : 'i';
        case 0x00CD: case 0x00ED: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x00CD ? 'I' : 'i';
        case 0x00CE: case 0x00EE: *out_accent = DISPLAY__ACCENT_CIRCUMFLEX; return c == 0x00CE ? 'I' : 'i';
        case 0x00CF: case 0x00EF: *out_accent = DISPLAY__ACCENT_DIAERESIS; return c == 0x00CF ? 'I' : 'i';
        case 0x00D1: case 0x00F1: *out_accent = DISPLAY__ACCENT_TILDE; return c == 0x00D1 ? 'N' : 'n';
        case 0x00D2: case 0x00F2: *out_accent = DISPLAY__ACCENT_GRAVE; return c == 0x00D2 ? 'O' : 'o';
        case 0x00D3: case 0x00F3: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x00D3 ? 'O' : 'o';
        case 0x00D4: case 0x00F4: *out_accent = DISPLAY__ACCENT_CIRCUMFLEX; return c == 0x00D4 ? 'O' : 'o';
        case 0x00D5: case 0x00F5: *out_accent = DISPLAY__ACCENT_TILDE; return c == 0x00D5 ? 'O' : 'o';
        case 0x00D6: case 0x00F6: *out_accent = DISPLAY__ACCENT_DIAERESIS; return c == 0x00D6 ? 'O' : 'o';
        case 0x00D9: case 0x00F9: *out_accent = DISPLAY__ACCENT_GRAVE; return c == 0x00D9 ? 'U' : 'u';
        case 0x00DA: case 0x00FA: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x00DA ? 'U' : 'u';
        case 0x00DB: case 0x00FB: *out_accent = DISPLAY__ACCENT_CIRCUMFLEX; return c == 0x00DB ? 'U' : 'u';
        case 0x00DC: case 0x00FC: *out_accent = DISPLAY__ACCENT_DIAERESIS; return c == 0x00DC ? 'U' : 'u';
        case 0x00DD: case 0x00FD: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x00DD ? 'Y' : 'y';
        case 0x0102: case 0x0103: *out_accent = DISPLAY__ACCENT_BREVE; return c == 0x0102 ? 'A' : 'a';
        case 0x0104: case 0x0105: *out_accent = DISPLAY__ACCENT_OGONEK; return c == 0x0104 ? 'A' : 'a';
        case 0x0106: case 0x0107: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x0106 ? 'C' : 'c';
        case 0x010E: *out_accent = DISPLAY__ACCENT_CARON; return 'D';
        case 0x010F: *out_accent = DISPLAY__ACCENT_APOSTROPHE; return 'd';
        case 0x010C: case 0x010D: case 0x0160: case 0x0161: case 0x017D: case 0x017E:
            *out_accent = DISPLAY__ACCENT_CARON; return c == 0x010C ? 'C' : c == 0x010D ? 'c' : c == 0x0160 ? 'S' : c == 0x0161 ? 's' : c == 0x017D ? 'Z' : 'z';
        case 0x0110: case 0x0111: *out_accent = DISPLAY__ACCENT_STROKE; return c == 0x0110 ? 'D' : 'd';
        case 0x0118: case 0x0119: *out_accent = DISPLAY__ACCENT_OGONEK; return c == 0x0118 ? 'E' : 'e';
        case 0x011A: case 0x011B: *out_accent = DISPLAY__ACCENT_CARON; return c == 0x011A ? 'E' : 'e';
        case 0x011E: case 0x011F: *out_accent = DISPLAY__ACCENT_BREVE; return c == 0x011E ? 'G' : 'g';
        case 0x0131: return 'i';
        case 0x0141: case 0x0142: *out_accent = DISPLAY__ACCENT_STROKE; return c == 0x0141 ? 'L' : 'l';
        case 0x0143: case 0x0144: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x0143 ? 'N' : 'n';
        case 0x0147: case 0x0148: *out_accent = DISPLAY__ACCENT_CARON; return c == 0x0147 ? 'N' : 'n';
        case 0x0150: case 0x0151: *out_accent = DISPLAY__ACCENT_DOUBLE_ACUTE; return c == 0x0150 ? 'O' : 'o';
        case 0x0154: case 0x0155: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x0154 ? 'R' : 'r';
        case 0x0158: case 0x0159: *out_accent = DISPLAY__ACCENT_CARON; return c == 0x0158 ? 'R' : 'r';
        case 0x015E: case 0x015F: *out_accent = DISPLAY__ACCENT_CEDILLA; return c == 0x015E ? 'S' : 's';
        case 0x0164: *out_accent = DISPLAY__ACCENT_CARON; return 'T';
        case 0x0165: *out_accent = DISPLAY__ACCENT_APOSTROPHE; return 't';
        case 0x016E: case 0x016F: *out_accent = DISPLAY__ACCENT_RING; return c == 0x016E ? 'U' : 'u';
        case 0x015A: case 0x015B: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x015A ? 'S' : 's';
        case 0x0179: case 0x017A: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x0179 ? 'Z' : 'z';
        case 0x017B: case 0x017C: *out_accent = DISPLAY__ACCENT_DOT_ABOVE; return c == 0x017B ? 'Z' : 'z';
        case 0x0218: case 0x0219: *out_accent = DISPLAY__ACCENT_CEDILLA; return c == 0x0218 ? 'S' : 's';
        case 0x021A: case 0x021B: *out_accent = DISPLAY__ACCENT_CEDILLA; return c == 0x021A ? 'T' : 't';
        case 0x013D: *out_accent = DISPLAY__ACCENT_CARON; return 'L';
        case 0x013E: *out_accent = DISPLAY__ACCENT_APOSTROPHE; return 'l';
        case 0x0139: case 0x013A: *out_accent = DISPLAY__ACCENT_ACUTE; return c == 0x0139 ? 'L' : 'l';
        case 0x2018: case 0x2019: return '\'';
        case 0x201C: case 0x201D: return '"';
        default: return c >= 32 && c <= 126 ? (char)c : '?';
    }
}

static void display__draw_accent(display__process_context_t *context, int16_t x, int16_t y, display__accent_t accent) {
    int16_t s = context->text_size;
    switch (accent) {
        case DISPLAY__ACCENT_NONE: return;
        case DISPLAY__ACCENT_ACUTE:
            display_internal__fill_rect(context, x + 3 * s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + 2 * s, y + s, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_GRAVE:
            display_internal__fill_rect(context, x + s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + 2 * s, y + s, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_CIRCUMFLEX:
            display_internal__fill_rect(context, x + 2 * s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + s, y + s, 3 * s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_TILDE:
            display_internal__fill_rect(context, x + s, y, 2 * s, s, context->text_color);
            display_internal__fill_rect(context, x + 3 * s, y + s, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_DIAERESIS:
            display_internal__fill_rect(context, x + s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + 3 * s, y, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_CEDILLA:
            display_internal__fill_rect(context, x + 2 * s, y + 7 * s, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_CARON:
        case DISPLAY__ACCENT_BREVE:
            display_internal__fill_rect(context, x + s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + 3 * s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + 2 * s, y + s, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_RING:
            display_internal__fill_rect(context, x + s, y, s, 2 * s, context->text_color);
            display_internal__fill_rect(context, x + 3 * s, y, s, 2 * s, context->text_color);
            return;
        case DISPLAY__ACCENT_STROKE:
            display_internal__fill_rect(context, x, y + 3 * s, 5 * s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_DOUBLE_ACUTE:
            display_internal__fill_rect(context, x + 2 * s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + 4 * s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + s, y + s, s, s, context->text_color);
            display_internal__fill_rect(context, x + 3 * s, y + s, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_OGONEK:
            display_internal__fill_rect(context, x + 3 * s, y + 7 * s, 2 * s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_DOT_ABOVE:
            display_internal__fill_rect(context, x + 2 * s, y, s, s, context->text_color);
            return;
        case DISPLAY__ACCENT_APOSTROPHE:
            display_internal__fill_rect(context, x + 4 * s, y, s, s, context->text_color);
            display_internal__fill_rect(context, x + 3 * s, y + s, s, s, context->text_color);
            return;
    }
}

static void display__draw_char(display__process_context_t *context, int16_t x, int16_t y, uint32_t codepoint) {
    display__accent_t accent;
    char base = display__latin_base(codepoint, &accent);
    const uint8_t *glyph = codepoint == 0x0131 || (codepoint >= 0x00EC && codepoint <= 0x00EF)
                               ? s_font_dotless_i
                               : display_internal__font_glyph(base);
    if (glyph == NULL) { return; }
    if (!context->text_bg_transparent) {
        display_internal__fill_rect(
            context,
            x,
            y,
            (DISPLAY__FONT_WIDTH + 1) * context->text_size,
            (DISPLAY__FONT_HEIGHT + 1) * context->text_size,
            context->text_bg_color
        );
    }
    for (int16_t row = 0; row <= DISPLAY__FONT_HEIGHT; ++row) {
        int16_t col = 0;
        while (col < DISPLAY__FONT_WIDTH) {
            while (col < DISPLAY__FONT_WIDTH && !(glyph[col] & (1 << row))) ++col;
            int16_t start = col;
            while (col < DISPLAY__FONT_WIDTH && (glyph[col] & (1 << row))) ++col;
            if (col > start) {
                display_internal__fill_rect(
                    context,
                    x + start * context->text_size,
                    y + row * context->text_size,
                    (col - start) * context->text_size,
                    context->text_size,
                    context->text_color
                );
            }
        }
    }
    display__draw_accent(context, x, y, accent);
}

static int32_t display__string_width(const display__process_context_t *context, const char *text) {
    int32_t width = 0;
    for (const char *p = text; *p != '\0';) {
        uint32_t codepoint;
        size_t length = display__utf8_decode(p, &codepoint);
        if (codepoint >= 0x20) width += (DISPLAY__FONT_WIDTH + 1) * context->text_size;
        p += length;
    }
    return width;
}

static void
display__draw_single_line(display__process_context_t *context, const char *text, int32_t x, int16_t y) {
    context->cursor_x = (int16_t)x;
    context->cursor_y = y;
    for (const char *p = text; *p != '\0';) {
        uint32_t codepoint;
        size_t length = display__utf8_decode(p, &codepoint);
        if (codepoint >= 0x20) {
            display__draw_char(context, context->cursor_x, context->cursor_y, codepoint);
            context->cursor_x += (DISPLAY__FONT_WIDTH + 1) * context->text_size;
        }
        p += length;
    }
}

static bruce_result_t
display__draw_aligned_string(const char *text, int16_t x, int16_t y, uint8_t alignment) {
    if (text == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    int32_t draw_x = x;
    int32_t width = display__string_width(context, text);
    if (alignment == 1) draw_x -= width / 2;
    else if (alignment == 2) draw_x -= width;
    display__draw_single_line(context, text, draw_x, y);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_text_color(bruce_display_color_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_color = color;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_text_bg_color(uint32_t color) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_bg_transparent = color >= 0x10000;
    if (!context->text_bg_transparent) context->text_bg_color = (bruce_display_color_t)color;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_text_size(uint8_t size) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->text_size = size < 1 ? 1 : (size > 8 ? 8 : size);
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__set_cursor(int16_t x, int16_t y) {
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    context->cursor_x = x;
    context->cursor_y = y;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__get_cursor(int16_t *x, int16_t *y) {
    if (x == NULL || y == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    *x = context->cursor_x;
    *y = context->cursor_y;
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__print(const char *text) {
    if (text == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    display__process_context_t *context;
    bruce_result_t result = display_internal__begin_draw(&context);
    if (result != BRUCE_OK) { return result; }
    for (const char *p = text; *p != '\0';) {
        uint32_t codepoint;
        size_t length = display__utf8_decode(p, &codepoint);
        if (codepoint == '\n') {
            context->cursor_x = 0;
            context->cursor_y += (DISPLAY__FONT_HEIGHT + 1) * context->text_size;
        } else if (codepoint == '\r') {
            context->cursor_x = 0;
        } else if (codepoint >= 0x20) {
            display__draw_char(context, context->cursor_x, context->cursor_y, codepoint);
            context->cursor_x += (DISPLAY__FONT_WIDTH + 1) * context->text_size;
        }
        p += length;
    }
    display_internal__unlock(context);
    return BRUCE_OK;
}

bruce_result_t display__println(const char *text) {
    bruce_result_t result = display__print(text);
    return result == BRUCE_OK ? display__print("\n") : result;
}

bruce_result_t display__draw_string(const char *text, int16_t x, int16_t y) {
    return display__draw_aligned_string(text, x, y, 0);
}

bruce_result_t display__draw_centre_string(const char *text, int16_t x, int16_t y) {
    return display__draw_aligned_string(text, x, y, 1);
}

bruce_result_t display__draw_right_string(const char *text, int16_t x, int16_t y) {
    return display__draw_aligned_string(text, x, y, 2);
}
