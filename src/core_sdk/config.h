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
 * persist immediately to bruce.json. Built-in modules always pass permission
 * checks; external ELF/JS tasks require `config`, except that protected
 * credentials are never exposed to external tasks.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* ------------------------------------------------------------------------ */
/* Permanently protected: an external (ELF/JS) task can NEVER read or write
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

const char *config__get_web_ui_user(void);
bruce_result_t config__set_web_ui_user(const char *value);
const char *config__get_web_ui_password(void);
bruce_result_t config__set_web_ui_password(const char *value);

/* ------------------------------------------------------------------------ */
/* `config`-permission-gated fields                                          */
/* ------------------------------------------------------------------------ */

/* Theme / display */
uint16_t config__get_pri_color(void);
bruce_result_t config__set_pri_color(uint16_t value);
uint16_t config__get_sec_color(void);
bruce_result_t config__set_sec_color(uint16_t value);
uint16_t config__get_bg_color(void);
bruce_result_t config__set_bg_color(uint16_t value);
const char *config__get_theme_path(void);
bruce_result_t config__set_theme_path(const char *value);
bool config__get_theme_on_sd(void);
bruce_result_t config__set_theme_on_sd(bool value);

/* Launcher */
const char *config__get_launcher_app(void);
bruce_result_t config__set_launcher_app(const char *value);

/* General settings */
int config__get_dimmer_set(void);
bruce_result_t config__set_dimmer_set(int value);
int config__get_bright(void);
bruce_result_t config__set_bright(int value);
bool config__get_automatic_time_update_via_ntp(void);
bruce_result_t config__set_automatic_time_update_via_ntp(bool value);
float config__get_tmz(void);
bruce_result_t config__set_tmz(float value);
bool config__get_dst(void);
bruce_result_t config__set_dst(bool value);
bool config__get_clock24hr(void);
bruce_result_t config__set_clock24hr(bool value);
bool config__get_sound_enabled(void);
bruce_result_t config__set_sound_enabled(bool value);
int config__get_sound_volume(void);
bruce_result_t config__set_sound_volume(int value);
bool config__get_wifi_at_startup(void);
bruce_result_t config__set_wifi_at_startup(bool value);
bool config__get_instant_boot(void);
bruce_result_t config__set_instant_boot(bool value);
const char *config__get_keyboard_lang(void);
bruce_result_t config__set_keyboard_lang(const char *value);
const bruce_config_hotkeys_t *config__get_hotkeys(void);
bruce_result_t config__set_hotkeys(const bruce_config_hotkey_t *values, size_t count);

/* LED */
int config__get_led_bright(void);
bruce_result_t config__set_led_bright(int value);
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
const char *config__get_startup_app_js_interpreter_file(void);
const char *config__get_wigle_basic_token(void);
const char *config__get_wdgwars_api_key(void);
bool config__get_dev_mode(void);
bruce_result_t config__set_dev_mode(bool value);
bool config__get_color_inverted(void);
bruce_result_t config__set_color_inverted(bool value);
