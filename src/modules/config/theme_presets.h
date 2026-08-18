#pragma once

/* Named theme palettes: policy that belongs to this module, not Core (see
 * core_sdk/config.h's theme section) - Core only stores and validates the
 * ten color_* roles, with no notion of a named theme. Two kinds of preset
 * live here:
 *
 *  - Legacy-style: only vary primary/secondary/background, the same three
 *    colors BrucePIO_legacy/src/core/settingsColor.h's UI_COLORS table let
 *    a user pick between. The other seven roles reuse Default's neutral
 *    surface/border/status set (or, for the two non-black legacy
 *    backgrounds, that same set lightened relative to their own
 *    background instead of pure black).
 *  - Community palettes: a complete, hand-picked set of all ten roles from
 *    each palette's own published spec (Catppuccin, Dracula, Nord,
 *    Gruvbox), converted from their 24-bit hex to RGB565.
 *
 * config_app__theme_apply_preset() (config_app.c) resolves a name from this
 * table to one config__set_colors() batch call; that's the only thing that
 * ever reads it. */

#include <stdint.h>

typedef struct {
    const char *name;
    uint16_t primary;
    uint16_t secondary;
    uint16_t background;
    uint16_t surface;
    uint16_t text;
    uint16_t text_muted;
    uint16_t border;
    uint16_t success;
    uint16_t warning;
    uint16_t error;
} config_app__theme_preset_t;

static const config_app__theme_preset_t CONFIG_APP__THEME_PRESETS[] = {
    /* clang-format off */
    /* name           primary secondary background surface text   textMuted border success warning error */
    {"Default",       0xA80F, 0x880F,   0x0000,     0x1082, 0xFFFF, 0x8410,  0x4208, 0x07E0, 0xFD20, 0xF800},
    {"Red",           0xF800, 0xC8E4,   0x0000,     0x1082, 0xFFFF, 0x8410,  0x4208, 0x07E0, 0xFD20, 0xF800},
    {"Orange",        0xFC40, 0xFE30,   0x0000,     0x1082, 0xFFFF, 0x8410,  0x4208, 0x07E0, 0xFD20, 0xF800},
    {"Yellow",        0xFFE0, 0xCE6A,   0x0000,     0x1082, 0xFFFF, 0x8410,  0x4208, 0x07E0, 0xFD20, 0xF800},
    {"Green",         0x07E0, 0x75E4,   0x0000,     0x1082, 0xFFFF, 0x8410,  0x4208, 0x07E0, 0xFD20, 0xF800},
    {"Blue",          0x001F, 0x0019,   0x0000,     0x1082, 0xFFFF, 0x8410,  0x4208, 0x07E0, 0xFD20, 0xF800},
    {"Purple",        0x7819, 0x93B9,   0x0000,     0x1082, 0xFFFF, 0x8410,  0x4208, 0x07E0, 0xFD20, 0xF800},
    {"Pink",          0xEF7C, 0xFE39,   0xE015,     0xF097, 0xFFFF, 0x8410,  0xFA1D, 0x07E0, 0xFD20, 0xF800},
    {"Dark Gray",     0x8430, 0x4228,   0x18A3,     0x2925, 0xFFFF, 0x8410,  0x5AAB, 0x07E0, 0xFD20, 0xF800},

    {"Catppuccin Mocha", 0xCD3E, 0x8DBF, 0x18E5, 0x3188, 0xCEBE, 0x7C33, 0x5ACE, 0xA714, 0xFF15, 0xF455},
    {"Dracula",          0xBC9F, 0xFBD8, 0x2946, 0x422B, 0xFFDE, 0x6394, 0x6394, 0x57CF, 0xFDCD, 0xFAAA},
    {"Nord",              0x8E1A, 0x8518, 0x29A8, 0x3A0A, 0xEF7E, 0x4AAD, 0x426B, 0xA5F1, 0xEE51, 0xBB0D},
    {"Gruvbox Dark",      0xFC03, 0xFDE5, 0x2945, 0x39C6, 0xEED6, 0xACD0, 0x62EA, 0xBDC4, 0xFDE5, 0xFA46},
    /* clang-format on */
};

#define CONFIG_APP__THEME_PRESET_COUNT (sizeof(CONFIG_APP__THEME_PRESETS) / sizeof(CONFIG_APP__THEME_PRESETS[0]))
