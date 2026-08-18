#pragma once

/*
 * Config API (public SDK surface).
 *
 * The Core owns one configuration singleton. Getters are type-safe and return
 * scalar values directly or read-only pointers into that singleton; they do
 * not allocate or copy. Pointers remain valid until the configuration is
 * changed, loaded, or reset and must never be freed by callers.
 *
 * Getters return zero, false, or NULL if initialization or permission checks
 * fail. Setters return BRUCE_OK or a BRUCE_ERR_* result, validate input, and
 * persist immediately to /config/bruce.conf. Built-in modules always pass permission
 * checks; external ELF/JS processes require `config`, except that protected
 * credentials are never exposed to external processes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* ------------------------------------------------------------------------ */
/* Permanently protected: an external (ELF/JS) process can NEVER read or write
 * these, even if it holds the `config` permission. Built-in modules are
 * unaffected. */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *ssid;
    const char *password;
} bruce_config_wifi_credential_t;

#define BRUCE_CONFIG_STARTUP_APP_MAX_COUNT 8
#define BRUCE_CONFIG_HOTKEY_MAX_COUNT 8
#define BRUCE_CONFIG_HOTKEY_MAX_LEN 32
#define BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN 64

typedef struct {
    const char *items[BRUCE_CONFIG_STARTUP_APP_MAX_COUNT];
    size_t count;
} bruce_config_startup_apps_t;

typedef struct {
    const char *key;
    /* AppRunner command line executed when the chord is pressed. */
    const char *action;
} bruce_config_hotkey_t;

typedef struct {
    bruce_config_hotkey_t items[BRUCE_CONFIG_HOTKEY_MAX_COUNT];
    size_t count;
} bruce_config_hotkeys_t;

const char *config__get_wifi_ap_ssid(void);
const char *config__get_wifi_ap_password(void);
bruce_result_t config__set_wifi_ap(const char *ssid, const char *password);

size_t config__wifi_credential_count(void);
const bruce_config_wifi_credential_t *config__wifi_credential_at(size_t index);
const bruce_config_wifi_credential_t *config__find_wifi_credential(const char *ssid);
bruce_result_t config__add_or_update_wifi_credential(const char *ssid, const char *password);

const char *config__get_wifi_mac(void);
bruce_result_t config__set_wifi_mac(const char *value);

/* ------------------------------------------------------------------------ */
/* `config`-permission-gated fields                                          */
/* ------------------------------------------------------------------------ */

/* Theme / display
 *
 * Ten RGB565 color roles: primary/secondary (accents), background
 * (full-screen canvas), surface (a raised panel/window on top of that
 * canvas), text/textMuted (body copy vs. de-emphasized copy), border
 * (dividers and window strokes), and success/warning/error (status
 * semantics). Core's own dialog renderer and built-in modules that draw
 * chrome (bruce_launcher, terminal, notification_service, ...) read these
 * instead of hard-coding colors. Core only stores and validates the ten
 * values - it has no notion of a named theme; a catalog of presets (legacy
 * single-accent sets, community palettes such as Dracula or Nord, ...) and
 * the UI/CLI to pick one is `modules/config`'s job (see its `theme`
 * subcommand), which resolves a name to either one of these ten setters
 * (a single role) or config__set_colors() below (a whole preset, applied
 * and persisted in one batch). */
uint16_t config__get_color_primary(void);
bruce_result_t config__set_color_primary(uint16_t value);
uint16_t config__get_color_secondary(void);
bruce_result_t config__set_color_secondary(uint16_t value);
uint16_t config__get_color_background(void);
bruce_result_t config__set_color_background(uint16_t value);
uint16_t config__get_color_surface(void);
bruce_result_t config__set_color_surface(uint16_t value);
uint16_t config__get_color_text(void);
bruce_result_t config__set_color_text(uint16_t value);
uint16_t config__get_color_text_muted(void);
bruce_result_t config__set_color_text_muted(uint16_t value);
uint16_t config__get_color_border(void);
bruce_result_t config__set_color_border(uint16_t value);
uint16_t config__get_color_success(void);
bruce_result_t config__set_color_success(uint16_t value);
uint16_t config__get_color_warning(void);
bruce_result_t config__set_color_warning(uint16_t value);
uint16_t config__get_color_error(void);
bruce_result_t config__set_color_error(uint16_t value);

/* All ten color_* roles at once, in the same order as the setters above.
 * Used by config__set_colors() below. */
typedef struct {
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
} bruce_config_theme_colors_t;

/* Applies all ten color_* roles and persists once, instead of the ten
 * separate writes ten config__set_color_*() calls would each incur. Callers
 * that set a whole theme at once (a named preset, a full WebUI theme
 * payload, ...) should prefer this over calling the individual setters in a
 * row; callers changing a single role should keep using that role's own
 * setter. */
bruce_result_t config__set_colors(const bruce_config_theme_colors_t *colors);

/* Parses a color written as CSS-style hex -- "RGB", "#RGB", "RRGGBB",
 * "#RRGGBB" (24-bit, downsampled to RGB565) or a bare 4-digit RGB565 value
 * such as "a80f" (native, used as-is) -- into `*out_rgb565`. Leading '#' is
 * optional and case is ignored. Returns false (leaving `*out_rgb565`
 * untouched) on a NULL/empty string or a length that matches none of those
 * forms. This is plain parsing, not a config field: it's exposed so any
 * caller that lets a user type a hex color (bruce.conf's theme loader, the
 * WebUI theme editor, `modules/config`'s theme subcommand, ...) gets
 * identical, forgiving parsing without duplicating it. */
bool config__parse_theme_color(const char *text, uint16_t *out_rgb565);
/* Controls whether Core retains a full RGB565 framebuffer. Changes apply
 * after reboot. When false, drawing is streamed directly to the panel and
 * framebuffer-dependent features such as snapshots are unavailable. */
bool config__get_display_buffered_rendering(void);
bruce_result_t config__set_display_buffered_rendering(bool value);
/* Display rotation in quarter turns clockwise (0..3). Changes apply after reboot. */
int config__get_display_rotation(void);
bruce_result_t config__set_display_rotation(int value);
/* Controls whether the full framebuffer is DMA-capable. Changes apply after
 * reboot and only affects buffered rendering. */
bool config__get_display_dma_framebuffer(void);
bruce_result_t config__set_display_dma_framebuffer(bool value);

/* Launcher */
const char *config__get_launcher(void);
bruce_result_t config__set_launcher(const char *value);

/* General settings */
int config__get_display_dim_timeout(void);
bruce_result_t config__set_display_dim_timeout(int value);
int config__get_display_brightness(void);
bruce_result_t config__set_display_brightness(int value);
bool config__get_time_automatic_update_via_ntp(void);
bruce_result_t config__set_time_automatic_update_via_ntp(bool value);
float config__get_time_timezone(void);
bruce_result_t config__set_time_timezone(float value);
bool config__get_time_dst(void);
bruce_result_t config__set_time_dst(bool value);
bool config__get_time_clock24hr(void);
bruce_result_t config__set_time_clock24hr(bool value);
bool config__get_sound_enabled(void);
bruce_result_t config__set_sound_enabled(bool value);
int config__get_sound_volume(void);
bruce_result_t config__set_sound_volume(int value);
const char *config__get_keyboard_lang(void);
bruce_result_t config__set_keyboard_lang(const char *value);
const bruce_config_hotkeys_t *config__get_hotkeys(void);
bruce_result_t config__set_hotkeys(const bruce_config_hotkey_t *values, size_t count);

/* LED */
bool config__get_led_enabled(void);
bruce_result_t config__set_led_enabled(bool value);
int config__get_led_brightness(void);
bruce_result_t config__set_led_brightness(int value);
uint32_t config__get_led_color(void);
bruce_result_t config__set_led_color(uint32_t value);
bool config__get_led_blink_enabled(void);
bruce_result_t config__set_led_blink_enabled(bool value);
int config__get_led_effect(void);
bruce_result_t config__set_led_effect(int value);
int config__get_led_effect_speed(void);
bruce_result_t config__set_led_effect_speed(int value);
int config__get_led_effect_direction(void);
bruce_result_t config__set_led_effect_direction(int value);

/* Misc */
const bruce_config_startup_apps_t *config__get_startup_apps(void);
bruce_result_t config__set_startup_apps(const char *const *values, size_t count);
/* Adding an existing key is a successful no-op. Removing a missing key returns
 * BRUCE_ERR_NOT_FOUND. */
bruce_result_t config__add_startup_app(const char *key);
bruce_result_t config__remove_startup_app(const char *key);
bool config__get_dev_mode(void);
bruce_result_t config__set_dev_mode(bool value);
