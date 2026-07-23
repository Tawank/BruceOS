#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/config.h"

/* Core-private legacy configuration representation. A5 exposes it through
 * field-specific getters/setters under src/core_sdk/config.h; this struct
 * and the whole-snapshot config__get()/config__set() below stay private and
 * are only used by Core itself (config.c's own JSON I/O, and other Core
 * modules such as core/wifi that need direct struct access). */
#define CONFIG__FILE_PATH "/bruce.json"

#define CONFIG__WIFI_SSID_MAX_LEN 32
#define CONFIG__WIFI_PASSWORD_MAX_LEN 64
#define CONFIG__WIFI_MAX_CREDENTIALS 8
#define CONFIG__WIFI_MAC_MAX_LEN 17

#define CONFIG__THEME_PATH_MAX_LEN 64
#define CONFIG__KEYBOARD_LANG_MAX_LEN 15
#define CONFIG__LAUNCHER_APP_MAX_LEN 64

#define CONFIG__WEBUI_USER_MAX_LEN 32
#define CONFIG__WEBUI_PASSWORD_MAX_LEN 64
#define CONFIG__WEBUI_SESSION_TOKEN_MAX_LEN 64
#define CONFIG__WEBUI_MAX_SESSIONS 5

#define CONFIG__EVIL_WIFI_MAX_NAMES 8
#define CONFIG__EVIL_ENDPOINT_MAX_LEN 32
#define CONFIG__EVIL_GATEWAY_IP_MAX_LEN 15

#define CONFIG__STARTUP_APP_MAX_LEN 32
#define CONFIG__STARTUP_APP_FILE_MAX_LEN 128
#define CONFIG__WIGLE_TOKEN_MAX_LEN 128
#define CONFIG__WDGWARS_KEY_MAX_LEN 72

#define CONFIG__DISABLED_MENU_MAX_LEN 32
#define CONFIG__DISABLED_MENU_MAX_COUNT 8

#define CONFIG__QR_CODE_MENU_NAME_MAX_LEN 32
#define CONFIG__QR_CODE_CONTENT_MAX_LEN 128
#define CONFIG__QR_CODE_MAX_ENTRIES 8

typedef enum {
    CONFIG__EVIL_PORTAL_FULL_PASSWORD = 0,
    CONFIG__EVIL_PORTAL_FIRST_LAST_CHAR = 1,
    CONFIG__EVIL_PORTAL_HIDE_PASSWORD = 2,
} config__evil_portal_password_mode_t;

typedef struct {
    const char *menuName;
    const char *content;
} config__qr_code_entry_t;

typedef struct {
    const char *getCredsEndpoint;
    const char *setSsidEndpoint;
    bool showEndpoints;
    bool allowSetSsid;
    bool allowGetCreds;
} config__evil_portal_endpoints_t;

/* Full snapshot of bruce.json. Field names preserve BrucePIO's BruceConfig casing.
 *
 * Every string is a heap-allocated, NUL-terminated `const char *` (no fixed
 * arrays). Ownership rules:
 *   - config__set() and the individual config__set_ and config__add_
 *     mutators always duplicate the strings you pass in; they never take
 *     ownership of your pointers, and you remain responsible for freeing
 *     your own inputs if you allocated them.
 *   - config__get() returns a shared, heap-allocated snapshot that
 *     YOU do not own.
 *   - config__wifi_credential_at() and config__find_wifi_credential()
 *     returns a shared, heap-allocated snapshot that
 *     YOU do not own.
 * The max-length macros below (CONFIG__*_MAX_LEN) are validation limits
 * enforced when a value is set, not fixed buffer sizes. */
typedef struct {
    /* Theme / display (RGB565 colors) */
    uint16_t priColor;
    uint16_t secColor;
    uint16_t bgColor;
    const char *themePath;
    bool themeOnSd;

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

    /* Evil portal */
    const char *evilWifiNames[CONFIG__EVIL_WIFI_MAX_NAMES];
    size_t evilWifiNameCount;
    config__evil_portal_endpoints_t evilPortalEndpoints;
    config__evil_portal_password_mode_t evilPortalPasswordMode;
    const char *evilPortalGatewayIp;

    /* Misc */
    const char *startupApp;
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

    config__qr_code_entry_t qrCodes[CONFIG__QR_CODE_MAX_ENTRIES];
    size_t qrCodeCount;
} config__t;

/* Mounts persistent storage, loads bruce.json, and creates it with defaults if absent. */
bool config__init(void);
bool config__load(void);
bool config__save(void);
bool config__factory_reset(void);

/* Compatibility initializer. New callers should use config__init(). */
void config__init_defaults(void);

/* Whole-struct access. config__set() validates, then persists to bruce.json.
 * config__get() returns a heap-allocated snapshot; release it with
 * config__free_snapshot() once you're done reading it. */
bool config__get(config__t *out);
bool config__set(const config__t *in);
void config__free_snapshot(config__t *snapshot);

/* Wi-Fi AP config, Wi-Fi client credentials, MAC, and Web UI credentials are
 * now public (and permanently protected from ELF/JS) - see
 * core_sdk/config.h's config__get_wifi_ap()/config__find_wifi_credential()/
 * etc. Core modules (e.g. core/wifi/wifi_common.c) call those same public
 * functions directly; there is no private duplicate. */

/* List helpers that mirror BruceConfig's dedicated mutators. */
bool config__add_qr_code_entry(const char *menu_name, const char *content);
bool config__remove_qr_code_entry(const char *menu_name);
bool config__add_disabled_menu(const char *value);
bool config__add_evil_wifi_name(const char *value);
bool config__remove_evil_wifi_name(const char *value);
bool config__add_web_ui_session(const char *token);
bool config__remove_web_ui_session(const char *token);
bool config__is_valid_web_ui_session(const char *token);
