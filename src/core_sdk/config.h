#pragma once

/*
 * Field-specific Config API (public SDK surface).
 *
 * A5 replaces whole-config external access with field-specific getters/
 * setters, `config` permission enforcement, and permanently-protected
 * fields.  Every fallible function below returns BRUCE_OK or a documented
 * BRUCE_ERR_* result (usually BRUCE_ERR_PERMISSION or
 * BRUCE_ERR_INVALID_ARGUMENT).  Setters validate their input the same way
 * BruceConfig historically did and persist immediately to bruce.json on
 * success.  Built-in modules always pass every check; only external (ELF/JS)
 * tasks are ever denied.
 *
 * String getters copy a NUL-terminated value into a caller-owned buffer
 * (truncating to fit) and never allocate, with one legacy exception:
 * `config__get_launcher_app()` predates A5 and still returns a heap-
 * allocated copy the caller must free().
 *
 * Not every bruce.json field has a public accessor yet (e.g. QR codes,
 * disabled menus, evil-portal settings, Web UI sessions, and BadUSB-BLE
 * options are still core-private). Add more following the same pattern as a
 * built-in module needs them - see core/config/config.c.
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
    /* Heap-allocated; free both with config__free_wifi_credential(). */
    const char *ssid;
    const char *password;
} bruce_config_wifi_credential_t;

bruce_result_t config__get_wifi_ap(char *ssid_out, size_t ssid_size, char *password_out, size_t password_size);
bruce_result_t config__set_wifi_ap(const char *ssid, const char *password);

bruce_result_t config__wifi_credential_count(size_t *out_count);
bruce_result_t config__wifi_credential_at(size_t index, bruce_config_wifi_credential_t *out_credential);
bruce_result_t config__find_wifi_credential(const char *ssid, bruce_config_wifi_credential_t *out_credential);
void config__free_wifi_credential(bruce_config_wifi_credential_t *credential);
bruce_result_t config__add_or_update_wifi_credential(const char *ssid, const char *password);

bruce_result_t config__get_wifi_mac(char *out, size_t out_size);
bruce_result_t config__set_wifi_mac(const char *value);

bruce_result_t config__get_web_ui_user(char *out, size_t out_size);
bruce_result_t config__set_web_ui_user(const char *value);
bruce_result_t config__get_web_ui_password(char *out, size_t out_size);
bruce_result_t config__set_web_ui_password(const char *value);

/* ------------------------------------------------------------------------ */
/* `config`-permission-gated fields                                          */
/* ------------------------------------------------------------------------ */

/* Theme / display */
bruce_result_t config__get_pri_color(uint16_t *out);
bruce_result_t config__set_pri_color(uint16_t value);
bruce_result_t config__get_sec_color(uint16_t *out);
bruce_result_t config__set_sec_color(uint16_t value);
bruce_result_t config__get_bg_color(uint16_t *out);
bruce_result_t config__set_bg_color(uint16_t value);
bruce_result_t config__get_theme_path(char *out, size_t out_size);
bruce_result_t config__set_theme_path(const char *value);
bruce_result_t config__get_theme_on_sd(bool *out);
bruce_result_t config__set_theme_on_sd(bool value);

/* Launcher */
char *config__get_launcher_app(void);
bruce_result_t config__set_launcher_app(const char *value);

/* General settings */
bruce_result_t config__get_dimmer_set(int *out);
bruce_result_t config__set_dimmer_set(int value);
bruce_result_t config__get_bright(int *out);
bruce_result_t config__set_bright(int value);
bruce_result_t config__get_automatic_time_update_via_ntp(bool *out);
bruce_result_t config__set_automatic_time_update_via_ntp(bool value);
bruce_result_t config__get_tmz(float *out);
bruce_result_t config__set_tmz(float value);
bruce_result_t config__get_dst(bool *out);
bruce_result_t config__set_dst(bool value);
bruce_result_t config__get_clock24hr(bool *out);
bruce_result_t config__set_clock24hr(bool value);
bruce_result_t config__get_sound_enabled(bool *out);
bruce_result_t config__set_sound_enabled(bool value);
bruce_result_t config__get_sound_volume(int *out);
bruce_result_t config__set_sound_volume(int value);
bruce_result_t config__get_wifi_at_startup(bool *out);
bruce_result_t config__set_wifi_at_startup(bool value);
bruce_result_t config__get_instant_boot(bool *out);
bruce_result_t config__set_instant_boot(bool value);
bruce_result_t config__get_keyboard_lang(char *out, size_t out_size);
bruce_result_t config__set_keyboard_lang(const char *value);

/* LED */
bruce_result_t config__get_led_bright(int *out);
bruce_result_t config__set_led_bright(int value);
bruce_result_t config__get_led_color(uint32_t *out);
bruce_result_t config__set_led_color(uint32_t value);
bruce_result_t config__get_led_blink_enabled(bool *out);
bruce_result_t config__set_led_blink_enabled(bool value);
bruce_result_t config__get_led_effect(int *out);
bruce_result_t config__set_led_effect(int value);
bruce_result_t config__get_led_effect_speed(int *out);
bruce_result_t config__set_led_effect_speed(int value);
bruce_result_t config__get_led_effect_direction(int *out);
bruce_result_t config__set_led_effect_direction(int value);

/* Misc */
bruce_result_t config__get_dev_mode(bool *out);
bruce_result_t config__set_dev_mode(bool value);
bruce_result_t config__get_color_inverted(bool *out);
bruce_result_t config__set_color_inverted(bool value);
bruce_result_t config__get_startup_app(char *out, size_t out_size);
bruce_result_t config__set_startup_app(const char *value);
