#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/config.h"

/* Core-private representation of the configuration singleton. */
#define CONFIG__DIRECTORY "/config"
#define CONFIG__FILE_PATH CONFIG__DIRECTORY "/bruce.json"

#define CONFIG__WIFI_SSID_MAX_LEN 32
#define CONFIG__WIFI_PASSWORD_MAX_LEN 64
#define CONFIG__WIFI_MAX_CREDENTIALS 8
#define CONFIG__WIFI_MAC_MAX_LEN 17

#define CONFIG__THEME_PATH_MAX_LEN 64
#define CONFIG__KEYBOARD_LANG_MAX_LEN 15
#define CONFIG__LAUNCHER_APP_MAX_LEN 64
#define CONFIG__HOTKEY_MAX_LEN BRUCE_CONFIG_HOTKEY_MAX_LEN
#define CONFIG__HOTKEY_ACTION_MAX_LEN BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN
#define CONFIG__HOTKEY_MAX_COUNT BRUCE_CONFIG_HOTKEY_MAX_COUNT

#define CONFIG__WEBUI_USER_MAX_LEN 32
#define CONFIG__WEBUI_PASSWORD_MAX_LEN 64
#define CONFIG__WEBUI_SESSION_TOKEN_MAX_LEN 64
#define CONFIG__WEBUI_MAX_SESSIONS 5

#define CONFIG__STARTUP_APP_MAX_LEN 32
#define CONFIG__STARTUP_APP_MAX_COUNT BRUCE_CONFIG_STARTUP_APP_MAX_COUNT
#define CONFIG__STARTUP_APP_FILE_MAX_LEN 128
#define CONFIG__WIGLE_TOKEN_MAX_LEN 128
#define CONFIG__WDGWARS_KEY_MAX_LEN 72

#define CONFIG__DISABLED_MENU_MAX_LEN 32
#define CONFIG__DISABLED_MENU_MAX_COUNT 8

/* In-memory bruce.json representation. Field names preserve BrucePIO's BruceConfig casing.
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
    uint16_t priColor;
    uint16_t secColor;
    uint16_t bgColor;
    const char *themePath;
    bool themeOnSd;
    bool displayBufferedRendering;
    bool displayDmaFramebuffer;

    const char *launcherApp;

    /* General settings */
    int dimmerSet;
    int bright;
    bool automaticTimeUpdateViaNTP;
    float tmz;
    bool dst;
    bool clock24hr;
    int soundEnabled;
    int soundVolume;
    int wifiAtStartup;
    int instantBoot;
    const char *keyboardLang; /* "QWERTY" | "AZERTY" | "QWERTZ" */
    bruce_config_hotkeys_t hotkeys;

    int ledBright;
    uint32_t ledColor;
    int ledBlinkEnabled;
    int ledEffect;
    int ledEffectSpeed;
    int ledEffectDirection;

    /* Web UI */
    const char *webUIUser;
    const char *webUIPassword;
    const char *webUISessions[CONFIG__WEBUI_MAX_SESSIONS];
    size_t webUISessionCount;

    /* Wi-Fi */
    const char *wifiApSsid;
    const char *wifiApPassword;
    bruce_config_wifi_credential_t wifiCredentials[CONFIG__WIFI_MAX_CREDENTIALS];
    size_t wifiCredentialCount;
    const char *wifiMAC;
    bool terminalLog;

    /* Misc */
    bruce_config_startup_apps_t startupApps;
    const char *startupAppJSInterpreterFile;
    const char *wigleBasicToken;
    const char *wdgwarsApiKey;
    int devMode;
    int colorInverted;

    int badUSBBLEKeyboardLayout;
    uint16_t badUSBBLEKeyDelay;
    bool badUSBBLEShowOutput;

    const char *disabledMenus[CONFIG__DISABLED_MENU_MAX_COUNT];
    size_t disabledMenuCount;

} config__t;

/* Loads /config/bruce.json and creates it with defaults if absent. */
bool config__init(void);
bool config__load(void);
bool config__save(void);
bool config__factory_reset(void);

/* Reads settings needed by Core audio without applying app config permission. */
void config__get_audio_settings(bool *enabled, int *volume);

/* Compatibility initializer. New callers should use config__init(). */
void config__init_defaults(void);

/* Wi-Fi AP config, Wi-Fi client credentials, MAC, and Web UI credentials are
 * public but permanently protected from ELF/JS. See core_sdk/config.h. */

/* Core-private list helpers. */
bool config__add_disabled_menu(const char *value);
bool config__add_web_ui_session(const char *token);
