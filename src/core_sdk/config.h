#pragma once

/**
 * @brief Bruce Config API.
 *
 * The Core owns one configuration singleton. Getters are type-safe and
 * return scalar values directly or read-only pointers into that singleton;
 * they do not allocate or copy. Pointers remain valid until the
 * configuration is changed, loaded, or reset and must never be freed by
 * callers.
 *
 * Getters return zero, false, or NULL if initialization or permission
 * checks fail. Setters return BRUCE_OK or a BRUCE_ERR_* result, validate
 * input, and persist immediately to /config/bruce.conf. Built-in modules
 * always pass permission checks; external ELF/JS processes require
 * `config`, except that protected credentials are never exposed to
 * external processes.
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

/**
 * @brief Configured Wi-Fi AP SSID.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
const char *config__get_wifi_ap_ssid(void);

/**
 * @brief Configured Wi-Fi AP password.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
const char *config__get_wifi_ap_password(void);

/**
 * @brief Sets the Wi-Fi AP SSID and password.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @param ssid New AP SSID.
 * @param password New AP password.
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
bruce_result_t config__set_wifi_ap(const char *ssid, const char *password);

/**
 * @brief Number of saved Wi-Fi station credentials.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
size_t config__wifi_credential_count(void);

/**
 * @brief Reads a saved Wi-Fi station credential by index.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @param index Zero-based index, below config__wifi_credential_count().
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
const bruce_config_wifi_credential_t *config__wifi_credential_at(size_t index);

/**
 * @brief Finds a saved Wi-Fi station credential by SSID.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @param ssid SSID to look up. Returns NULL if not found.
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
const bruce_config_wifi_credential_t *config__find_wifi_credential(const char *ssid);

/**
 * @brief Adds a new saved Wi-Fi station credential, or updates an existing one with the same SSID.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @param ssid SSID of the credential to add or update.
 * @param password Password to store for this SSID.
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
bruce_result_t config__add_or_update_wifi_credential(const char *ssid, const char *password);

/**
 * @brief Configured Wi-Fi MAC address override, if any.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
const char *config__get_wifi_mac(void);

/**
 * @brief Sets the Wi-Fi MAC address override.
 *
 * Permanently protected: never exposed to external (ELF/JS) processes, even
 * with the `config` permission. Built-in modules are unaffected.
 *
 * @param value New MAC address override.
 * @permission none (permanently protected -- inaccessible to external processes even with `config`)
 */
bruce_result_t config__set_wifi_mac(const char *value);

/* ------------------------------------------------------------------------ */
/* `config`-permission-gated fields                                          */
/* ------------------------------------------------------------------------ */

/**
 * @brief Theme / display colors.
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
 * and persisted in one batch).
 *
 * @permission config
 */
uint16_t config__get_color_primary(void);

/**
 * @brief Sets the primary accent color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_primary(uint16_t value);

/** @brief Secondary accent color. @permission config */
uint16_t config__get_color_secondary(void);

/**
 * @brief Sets the secondary accent color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_secondary(uint16_t value);

/** @brief Full-screen canvas background color. @permission config */
uint16_t config__get_color_background(void);

/**
 * @brief Sets the background color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_background(uint16_t value);

/** @brief Raised panel/window surface color. @permission config */
uint16_t config__get_color_surface(void);

/**
 * @brief Sets the surface color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_surface(uint16_t value);

/** @brief Body copy text color. @permission config */
uint16_t config__get_color_text(void);

/**
 * @brief Sets the body text color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_text(uint16_t value);

/** @brief De-emphasized text color. @permission config */
uint16_t config__get_color_text_muted(void);

/**
 * @brief Sets the muted text color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_text_muted(uint16_t value);

/** @brief Divider/window stroke color. @permission config */
uint16_t config__get_color_border(void);

/**
 * @brief Sets the border color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_border(uint16_t value);

/** @brief Success-status color. @permission config */
uint16_t config__get_color_success(void);

/**
 * @brief Sets the success-status color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_success(uint16_t value);

/** @brief Warning-status color. @permission config */
uint16_t config__get_color_warning(void);

/**
 * @brief Sets the warning-status color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
bruce_result_t config__set_color_warning(uint16_t value);

/** @brief Error-status color. @permission config */
uint16_t config__get_color_error(void);

/**
 * @brief Sets the error-status color.
 *
 * @param value New RGB565 color value.
 * @permission config
 */
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

/**
 * @brief Applies all ten color_* roles and persists once.
 *
 * Instead of the ten separate writes ten config__set_color_*() calls would
 * each incur. Callers that set a whole theme at once (a named preset, a
 * full WebUI theme payload, ...) should prefer this over calling the
 * individual setters in a row; callers changing a single role should keep
 * using that role's own setter.
 *
 * @param colors All ten color roles to apply and persist together.
 * @permission config
 */
bruce_result_t config__set_colors(const bruce_config_theme_colors_t *colors);

/**
 * @brief Parses a color written as CSS-style hex.
 *
 * "RGB", "#RGB", "RRGGBB", "#RRGGBB" (24-bit, downsampled to RGB565) or a
 * bare 4-digit RGB565 value such as "a80f" (native, used as-is) -- into
 * `*out_rgb565`. Leading '#' is optional and case is ignored. Returns false
 * (leaving `*out_rgb565` untouched) on a NULL/empty string or a length that
 * matches none of those forms. This is plain parsing, not a config field:
 * it's exposed so any caller that lets a user type a hex color
 * (bruce.conf's theme loader, the WebUI theme editor, `modules/config`'s
 * theme subcommand, ...) gets identical, forgiving parsing without
 * duplicating it.
 *
 * @param text Hex color text to parse.
 * @param out_rgb565 Receives the parsed RGB565 value on success.
 */
bool config__parse_theme_color(const char *text, uint16_t *out_rgb565);

/**
 * @brief Controls whether Core retains a full RGB565 framebuffer.
 *
 * Changes apply after reboot. When false, drawing is streamed directly to
 * the panel and framebuffer-dependent features such as snapshots are
 * unavailable.
 *
 * @permission config
 */
bool config__get_display_buffered_rendering(void);

/**
 * @brief Sets whether Core retains a full RGB565 framebuffer.
 *
 * Changes apply after reboot.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_display_buffered_rendering(bool value);

/**
 * @brief Display rotation in quarter turns clockwise (0..3).
 *
 * Changes apply after reboot.
 *
 * @permission config
 */
int config__get_display_rotation(void);

/**
 * @brief Sets the display rotation.
 *
 * Changes apply after reboot.
 *
 * @param value Quarter turns clockwise (0..3).
 * @permission config
 */
bruce_result_t config__set_display_rotation(int value);

/**
 * @brief Controls whether the full framebuffer is DMA-capable.
 *
 * Changes apply after reboot and only affects buffered rendering.
 *
 * @permission config
 */
bool config__get_display_dma_framebuffer(void);

/**
 * @brief Sets whether the full framebuffer is DMA-capable.
 *
 * Changes apply after reboot and only affects buffered rendering.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_display_dma_framebuffer(bool value);

/**
 * @brief Launcher command run at startup.
 *
 * @permission config
 */
const char *config__get_launcher(void);

/**
 * @brief Sets the launcher command run at startup.
 *
 * @param value New launcher command line.
 * @permission config
 */
bruce_result_t config__set_launcher(const char *value);

/** @brief Screen dim timeout, in seconds. @permission config */
int config__get_display_dim_timeout(void);

/**
 * @brief Sets the screen dim timeout.
 *
 * @param value New timeout in seconds.
 * @permission config
 */
bruce_result_t config__set_display_dim_timeout(int value);

/** @brief Display brightness (0-100). @permission config */
int config__get_display_brightness(void);

/**
 * @brief Sets the display brightness.
 *
 * @param value New brightness (0-100).
 * @permission config
 */
bruce_result_t config__set_display_brightness(int value);

/** @brief Whether the clock automatically syncs via NTP. @permission config */
bool config__get_time_automatic_update_via_ntp(void);

/**
 * @brief Sets whether the clock automatically syncs via NTP.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_time_automatic_update_via_ntp(bool value);

/** @brief UTC offset in hours (may be fractional). @permission config */
float config__get_time_timezone(void);

/**
 * @brief Sets the UTC offset.
 *
 * @param value New UTC offset in hours (may be fractional).
 * @permission config
 */
bruce_result_t config__set_time_timezone(float value);

/** @brief Whether the manual one-hour DST adjustment is applied. @permission config */
bool config__get_time_dst(void);

/**
 * @brief Sets the manual one-hour DST adjustment.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_time_dst(bool value);

/** @brief Whether times are displayed in 24-hour format. @permission config */
bool config__get_time_clock24hr(void);

/**
 * @brief Sets whether times are displayed in 24-hour format.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_time_clock24hr(bool value);

/** @brief Whether sound is enabled. @permission config */
bool config__get_sound_enabled(void);

/**
 * @brief Sets whether sound is enabled.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_sound_enabled(bool value);

/** @brief Sound volume (0-100). @permission config */
int config__get_sound_volume(void);

/**
 * @brief Sets the sound volume.
 *
 * @param value New volume (0-100).
 * @permission config
 */
bruce_result_t config__set_sound_volume(int value);

/** @brief Configured on-screen keyboard language. @permission config */
const char *config__get_keyboard_lang(void);

/**
 * @brief Sets the on-screen keyboard language.
 *
 * @param value New keyboard language code.
 * @permission config
 */
bruce_result_t config__set_keyboard_lang(const char *value);

/** @brief Configured hotkey chords. @permission config */
const bruce_config_hotkeys_t *config__get_hotkeys(void);

/**
 * @brief Replaces the whole hotkey list.
 *
 * @param values New hotkey entries.
 * @param count Number of entries in values.
 * @permission config
 */
bruce_result_t config__set_hotkeys(const bruce_config_hotkey_t *values, size_t count);

/** @brief Whether the status LED is enabled. @permission config */
bool config__get_led_enabled(void);

/**
 * @brief Sets whether the status LED is enabled.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_led_enabled(bool value);

/** @brief Status LED brightness (0-100). @permission config */
int config__get_led_brightness(void);

/**
 * @brief Sets the status LED brightness.
 *
 * @param value New brightness (0-100).
 * @permission config
 */
bruce_result_t config__set_led_brightness(int value);

/** @brief Status LED color. @permission config */
uint32_t config__get_led_color(void);

/**
 * @brief Sets the status LED color.
 *
 * @param value New color value.
 * @permission config
 */
bruce_result_t config__set_led_color(uint32_t value);

/** @brief Whether the status LED blinks. @permission config */
bool config__get_led_blink_enabled(void);

/**
 * @brief Sets whether the status LED blinks.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_led_blink_enabled(bool value);

/** @brief Selected status LED effect. @permission config */
int config__get_led_effect(void);

/**
 * @brief Sets the status LED effect.
 *
 * @param value New effect identifier.
 * @permission config
 */
bruce_result_t config__set_led_effect(int value);

/** @brief Status LED effect speed. @permission config */
int config__get_led_effect_speed(void);

/**
 * @brief Sets the status LED effect speed.
 *
 * @param value New effect speed.
 * @permission config
 */
bruce_result_t config__set_led_effect_speed(int value);

/** @brief Status LED effect direction. @permission config */
int config__get_led_effect_direction(void);

/**
 * @brief Sets the status LED effect direction.
 *
 * @param value New effect direction.
 * @permission config
 */
bruce_result_t config__set_led_effect_direction(int value);

/**
 * @brief Startup app list.
 *
 * @permission config
 */
const bruce_config_startup_apps_t *config__get_startup_apps(void);

/**
 * @brief Replaces the whole startup app list.
 *
 * @param values New startup app keys.
 * @param count Number of entries in values.
 * @permission config
 */
bruce_result_t config__set_startup_apps(const char *const *values, size_t count);

/**
 * @brief Adds an app to the startup list.
 *
 * Adding an existing key is a successful no-op.
 *
 * @param key App key to add.
 * @permission config
 */
bruce_result_t config__add_startup_app(const char *key);

/**
 * @brief Removes an app from the startup list.
 *
 * Returns BRUCE_ERR_NOT_FOUND if the key isn't present.
 *
 * @param key App key to remove.
 * @permission config
 */
bruce_result_t config__remove_startup_app(const char *key);

/**
 * @brief Developer-mode flag.
 *
 * @permission config
 */
bool config__get_dev_mode(void);

/**
 * @brief Sets the developer-mode flag.
 *
 * @param value New setting.
 * @permission config
 */
bruce_result_t config__set_dev_mode(bool value);
