#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/config.h"

/* Core-private representation of the configuration singleton. */
#define CONFIG__DIRECTORY "/config"
#define CONFIG__FILE_PATH CONFIG__DIRECTORY "/bruce.conf"

#define CONFIG__WIFI_SSID_MAX_LEN 32
#define CONFIG__WIFI_PASSWORD_MAX_LEN 64
#define CONFIG__WIFI_MAX_CREDENTIALS 8
#define CONFIG__WIFI_MAC_MAX_LEN 17

#define CONFIG__KEYBOARD_LANG_MAX_LEN 15
#define CONFIG__LAUNCHER_APP_MAX_LEN 64
#define CONFIG__HOTKEY_MAX_LEN BRUCE_CONFIG_HOTKEY_MAX_LEN
#define CONFIG__HOTKEY_ACTION_MAX_LEN BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN
#define CONFIG__HOTKEY_MAX_COUNT BRUCE_CONFIG_HOTKEY_MAX_COUNT

#define CONFIG__STARTUP_APP_MAX_LEN 32
#define CONFIG__STARTUP_APP_MAX_COUNT BRUCE_CONFIG_STARTUP_APP_MAX_COUNT

/* In-memory bruce.conf representation. Field names mirror the JSON groups.
 *
 * Every string is a heap-allocated, NUL-terminated `const char *` (no fixed
 * arrays). Ownership rules:
 *   - config__set_ and config__add_ mutators duplicate string inputs; they
 *     never take ownership of caller memory.
 *   - getters returning pointers expose singleton-owned memory. Callers must
 *     not mutate or free it.
 * The max-length macros below (CONFIG__*_MAX_LEN) are validation limits
 * enforced when a value is set, not fixed buffer sizes. */
typedef struct {
    /* Theme / display (RGB565 colors) */
    uint16_t colorPrimary;
    uint16_t colorSecondary;
    uint16_t colorBackground;
    uint16_t colorSurface;
    uint16_t colorText;
    uint16_t colorTextMuted;
    uint16_t colorBorder;
    uint16_t colorSuccess;
    uint16_t colorWarning;
    uint16_t colorError;
    int displayRotation;
    bool displayBufferedRendering;
    bool displayDmaFramebuffer;

    const char *launcher;

    /* General settings */
    int displayDimTimeout;
    int displayBrightness;
    bool timeAutomaticUpdateViaNTP;
    float timeTimezone;
    bool timeDst;
    bool timeClock24hr;
    bool soundEnabled;
    int soundVolume;
    const char *keyboardLang; /* "QWERTY" | "AZERTY" | "QWERTZ" */
    bruce_config_hotkeys_t hotkeys;

    int ledBrightness;
    bool ledEnabled;
    uint32_t ledColor;
    int ledBlinkEnabled;
    int ledEffect;
    int ledEffectSpeed;
    int ledEffectDirection;

    /* Wi-Fi */
    const char *wifiApSsid;
    const char *wifiApPassword;
    bruce_config_wifi_credential_t wifiCredentials[CONFIG__WIFI_MAX_CREDENTIALS];
    size_t wifiCredentialCount;
    const char *wifiMac;

    /* Misc */
    bruce_config_startup_apps_t startup;
    int devMode;

} config__t;

/* Loads /config/bruce.conf and creates it with defaults if absent. */
bool config__init(void);
bool config__load(void);
bool config__save(void);
bool config__factory_reset(void);

/* Reads settings needed by Core audio without applying app config permission. */
void config__get_audio_settings(bool *enabled, int *volume);

/* Reads all ten colors Core's own dialog renderer needs to draw any UI
 * chrome (title bars, footers, backgrounds, bordered windows, and a
 * success/warning/error dialog__message() accent) without applying app
 * config permission - see config__get_colors_internal() in config.c for
 * why: the public core_sdk/config.h getters return black (0) when the
 * calling process lacks "config", which would make dialog.c draw every
 * screen - including the permission-request dialog itself - in unreadable
 * black on black. */
void config__get_colors_internal(
    uint16_t *pri, uint16_t *sec, uint16_t *bg, uint16_t *surface, uint16_t *text, uint16_t *text_muted,
    uint16_t *border, uint16_t *success, uint16_t *warning, uint16_t *error
);

/* Compatibility initializer. New callers should use config__init(). */
void config__init_defaults(void);

/* Wi-Fi AP config, Wi-Fi client credentials, and MAC are public but
 * permanently protected from ELF/JS. See core_sdk/config.h. WebUI's own
 * credentials/sessions live in /config/webui.conf; see core_sdk/app_config.h. */
