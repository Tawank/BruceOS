#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/storage/storage.h"
#include "core/process/process.h"
#include "core_sdk/config.h"
#include "core_sdk/permission.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_config_mutex_storage;
static SemaphoreHandle_t s_config_mutex;
static portMUX_TYPE s_config_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_defaults_initialized;
static bool s_loaded;
static config__t s_config;

static void config__ensure_mutex(void) {
    if (s_config_mutex != NULL) return;
    portENTER_CRITICAL(&s_config_init_mux);
    if (s_config_mutex == NULL) { s_config_mutex = xSemaphoreCreateMutexStatic(&s_config_mutex_storage); }
    portEXIT_CRITICAL(&s_config_init_mux);
}

static void config__lock(void) {
    config__ensure_mutex();
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
}

static void config__unlock(void) { xSemaphoreGive(s_config_mutex); }

static bool config__valid_value(const char *value, size_t max_length, bool allow_empty) {
    return value != NULL && (allow_empty || value[0] != '\0') && strnlen(value, max_length + 1) <= max_length;
}

/* Heap-allocates a NUL-terminated copy of `value` (treating NULL as ""). */
static char *config__strdup(const char *value) {
    if (value == NULL) value = "";
    size_t length = strlen(value) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

/* Replaces *field with a heap copy of `value`, freeing the previous copy.
 * `value` is read before the old copy is freed, so it's safe to call this
 * with value == *field (i.e. re-duplicating a field in place). On
 * allocation failure the previous value is left untouched. */
static bool config__assign(const char **field, const char *value) {
    char *copy = config__strdup(value);
    if (copy == NULL) return false;
    free((void *)*field);
    *field = copy;
    return true;
}

static void config__release(const char **field) {
    free((void *)*field);
    *field = NULL;
}

static const char *config__or_empty(const char *value) { return value != NULL ? value : ""; }

/* ------------------------------------------------------------------------ */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------ */

/* Frees every heap string owned by cfg (safe to call on a zeroed struct). */
static void config__free_config(config__t *cfg) {
    config__release(&cfg->themePath);
    config__release(&cfg->launcherApp);
    config__release(&cfg->keyboardLang);
    for (size_t i = 0; i < CONFIG__HOTKEY_MAX_COUNT; ++i) {
        config__release(&cfg->hotkeys.items[i].key);
        config__release(&cfg->hotkeys.items[i].action);
    }
    config__release(&cfg->webUIUser);
    config__release(&cfg->webUIPassword);
    for (size_t i = 0; i < CONFIG__WEBUI_MAX_SESSIONS; ++i) config__release(&cfg->webUISessions[i]);
    config__release(&cfg->wifiApSsid);
    config__release(&cfg->wifiApPassword);
    for (size_t i = 0; i < CONFIG__WIFI_MAX_CREDENTIALS; ++i) {
        config__release(&cfg->wifiCredentials[i].ssid);
        config__release(&cfg->wifiCredentials[i].password);
    }
    config__release(&cfg->wifiMAC);
    for (size_t i = 0; i < CONFIG__STARTUP_APP_MAX_COUNT; ++i) config__release(&cfg->startupApps.items[i]);
    config__release(&cfg->startupAppJSInterpreterFile);
    config__release(&cfg->wigleBasicToken);
    config__release(&cfg->wdgwarsApiKey);
    for (size_t i = 0; i < CONFIG__DISABLED_MENU_MAX_COUNT; ++i) config__release(&cfg->disabledMenus[i]);
}

static void config__set_defaults(config__t *cfg) {
    config__free_config(cfg);
    memset(cfg, 0, sizeof(*cfg));

    cfg->priColor = 0xA80F;
    cfg->secColor = 0xA80F - 0x2000;
    cfg->bgColor = 0x0000;
    config__assign(&cfg->themePath, "");
    cfg->themeOnSd = false;
    cfg->displayDmaFramebuffer = true;
    config__assign(&cfg->launcherApp, "");

    cfg->dimmerSet = 60;
    cfg->bright = 100;
    cfg->automaticTimeUpdateViaNTP = true;
    cfg->tmz = 0;
    cfg->dst = false;
    cfg->clock24hr = true;
    cfg->soundEnabled = 1;
    cfg->soundVolume = 100;
    cfg->wifiAtStartup = 0;
    cfg->instantBoot = 0;
    config__assign(&cfg->keyboardLang, "QWERTY");
    config__assign(&cfg->hotkeys.items[0].key, "alt + tab");
    config__assign(&cfg->hotkeys.items[0].action, "process switch next");
    config__assign(&cfg->hotkeys.items[1].key, "ctrl + tab");
    config__assign(&cfg->hotkeys.items[1].action, "process preview");
    config__assign(&cfg->hotkeys.items[2].key, "ctrl + space");
    config__assign(&cfg->hotkeys.items[2].action, "launcher");
    cfg->hotkeys.count = 3;

    cfg->ledBright = 50;
    cfg->ledColor = 0x960064;
    cfg->ledBlinkEnabled = 1;
    cfg->ledEffect = 0;
    cfg->ledEffectSpeed = 5;
    cfg->ledEffectDirection = 1;

    config__assign(&cfg->webUIUser, "admin");
    config__assign(&cfg->webUIPassword, "bruce");

    config__assign(&cfg->wifiApSsid, "BruceNet");
    config__assign(&cfg->wifiApPassword, "brucenet");
    config__assign(&cfg->wifiMAC, "");
    cfg->terminalLog = true;

    static const char *const default_startup_apps[] = {
        "bootanimation",
        "input",
        "serial_commands",
        "launcher -s",
    };
    for (size_t i = 0; i < sizeof(default_startup_apps) / sizeof(default_startup_apps[0]); ++i) {
        config__assign(&cfg->startupApps.items[i], default_startup_apps[i]);
    }
    cfg->startupApps.count = sizeof(default_startup_apps) / sizeof(default_startup_apps[0]);
    config__assign(&cfg->startupAppJSInterpreterFile, "");
    config__assign(&cfg->wigleBasicToken, "");
    config__assign(&cfg->wdgwarsApiKey, "your 64-char hex key from wdgwars.pl/profile");
    cfg->devMode = 0;
    cfg->colorInverted = 1;

    cfg->badUSBBLEKeyboardLayout = 0;
    cfg->badUSBBLEKeyDelay = 10;
    cfg->badUSBBLEShowOutput = true;
}

/* ------------------------------------------------------------------------ */
/* Validation (mirrors BruceConfig::validateConfig)                         */
/* ------------------------------------------------------------------------ */

static void config__validate(config__t *cfg) {
    if (cfg->dimmerSet < 0) cfg->dimmerSet = 10;
    if (cfg->dimmerSet > 60) cfg->dimmerSet = 0;

    if (cfg->bright > 100) cfg->bright = 100;

    if (cfg->tmz < -12 || cfg->tmz > 14) cfg->tmz = 0;

    if (cfg->soundEnabled > 1) cfg->soundEnabled = 1;
    if (cfg->soundVolume > 100) cfg->soundVolume = 100;
    if (cfg->wifiAtStartup > 1) cfg->wifiAtStartup = 1;

    if (cfg->ledBright < 0) cfg->ledBright = 0;
    if (cfg->ledBright > 100) cfg->ledBright = 100;
    if (cfg->ledBlinkEnabled > 1) cfg->ledBlinkEnabled = 1;
    if (cfg->ledEffect < 0 || cfg->ledEffect > 9) cfg->ledEffect = 0;
#ifdef HAS_ENCODER_LED
    if (cfg->ledEffectSpeed > 11) cfg->ledEffectSpeed = 11;
#else
    if (cfg->ledEffectSpeed > 10) cfg->ledEffectSpeed = 10;
#endif
    if (cfg->ledEffectSpeed < 0) cfg->ledEffectSpeed = 1;
    if (cfg->ledEffectDirection > 1 || cfg->ledEffectDirection == 0) cfg->ledEffectDirection = 1;
    if (cfg->ledEffectDirection < -1) cfg->ledEffectDirection = -1;

    if (cfg->devMode > 1) cfg->devMode = 1;
    if (cfg->colorInverted > 1) cfg->colorInverted = 1;

    if (cfg->badUSBBLEKeyboardLayout < 0 || cfg->badUSBBLEKeyboardLayout > 13) {
        cfg->badUSBBLEKeyboardLayout = 0;
    }
    if (cfg->badUSBBLEKeyDelay > 500) cfg->badUSBBLEKeyDelay = 500;
}

/* ------------------------------------------------------------------------ */
/* JSON <-> config__t                                                        */
/* ------------------------------------------------------------------------ */

static void json_get_string(const cJSON *object, const char *key, const char **out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) config__assign(out, item->valuestring);
}

static void json_get_int(const cJSON *object, const char *key, int *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) *out = item->valueint;
}

static void json_get_uint16(const cJSON *object, const char *key, uint16_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) *out = (uint16_t)item->valueint;
}

static void json_get_float(const cJSON *object, const char *key, float *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) *out = (float)item->valuedouble;
}

static void json_get_bool(const cJSON *object, const char *key, bool *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsBool(item)) *out = cJSON_IsTrue(item);
}

static void json_get_hex16(const cJSON *object, const char *key, uint16_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
        *out = (uint16_t)strtoul(item->valuestring, NULL, 16);
}

static void json_get_hex32(const cJSON *object, const char *key, uint32_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
        *out = (uint32_t)strtoul(item->valuestring, NULL, 16);
}

static void config__clear_string_array(const char **array, size_t max_count) {
    for (size_t i = 0; i < max_count; ++i) config__release(&array[i]);
}

static void config__parse_json(config__t *cfg, const cJSON *root) {
    if (!cJSON_IsObject(root)) return;

    json_get_hex16(root, "priColor", &cfg->priColor);
    json_get_hex16(root, "secColor", &cfg->secColor);
    json_get_hex16(root, "bgColor", &cfg->bgColor);
    json_get_string(root, "themeFile", &cfg->themePath);
    json_get_bool(root, "themeOnSd", &cfg->themeOnSd);
    json_get_bool(root, "displayDmaFramebuffer", &cfg->displayDmaFramebuffer);
    json_get_string(root, "launcherApp", &cfg->launcherApp);

    json_get_int(root, "dimmerSet", &cfg->dimmerSet);
    json_get_int(root, "bright", &cfg->bright);
    json_get_bool(root, "automaticTimeUpdateViaNTP", &cfg->automaticTimeUpdateViaNTP);
    json_get_float(root, "tmz", &cfg->tmz);
    json_get_bool(root, "dst", &cfg->dst);
    json_get_bool(root, "clock24hr", &cfg->clock24hr);
    json_get_int(root, "soundEnabled", &cfg->soundEnabled);
    json_get_int(root, "soundVolume", &cfg->soundVolume);
    json_get_int(root, "wifiAtStartup", &cfg->wifiAtStartup);
    json_get_int(root, "instantBoot", &cfg->instantBoot);
    json_get_string(root, "keyboardLang", &cfg->keyboardLang);

    const cJSON *hotkeys = cJSON_GetObjectItemCaseSensitive(root, "hotkeys");
    if (cJSON_IsObject(hotkeys)) {
        for (size_t i = 0; i < CONFIG__HOTKEY_MAX_COUNT; ++i) {
            config__release(&cfg->hotkeys.items[i].key);
            config__release(&cfg->hotkeys.items[i].action);
        }
        cfg->hotkeys.count = 0;
        const cJSON *hotkey;
        cJSON_ArrayForEach(hotkey, hotkeys) {
            if (cfg->hotkeys.count >= CONFIG__HOTKEY_MAX_COUNT) break;
            if (hotkey->string == NULL || !cJSON_IsString(hotkey) || hotkey->valuestring == NULL ||
                !config__valid_value(hotkey->string, CONFIG__HOTKEY_MAX_LEN, false) ||
                !config__valid_value(hotkey->valuestring, CONFIG__HOTKEY_ACTION_MAX_LEN, false)) {
                continue;
            }
            bruce_config_hotkey_t *entry = &cfg->hotkeys.items[cfg->hotkeys.count];
            entry->key = config__strdup(hotkey->string);
            entry->action = config__strdup(hotkey->valuestring);
            if (entry->key != NULL && entry->action != NULL) {
                ++cfg->hotkeys.count;
            } else {
                config__release(&entry->key);
                config__release(&entry->action);
            }
        }
    }

    json_get_int(root, "ledBright", &cfg->ledBright);
    json_get_hex32(root, "ledColor", &cfg->ledColor);
    json_get_int(root, "ledBlinkEnabled", &cfg->ledBlinkEnabled);
    json_get_int(root, "ledEffect", &cfg->ledEffect);
    json_get_int(root, "ledEffectSpeed", &cfg->ledEffectSpeed);
    json_get_int(root, "ledEffectDirection", &cfg->ledEffectDirection);

    const cJSON *web_ui = cJSON_GetObjectItemCaseSensitive(root, "webUI");
    if (cJSON_IsObject(web_ui)) {
        json_get_string(web_ui, "user", &cfg->webUIUser);
        json_get_string(web_ui, "pwd", &cfg->webUIPassword);
    }

    const cJSON *web_ui_sessions = cJSON_GetObjectItemCaseSensitive(root, "webUISessions");
    if (cJSON_IsObject(web_ui_sessions)) {
        config__clear_string_array(cfg->webUISessions, CONFIG__WEBUI_MAX_SESSIONS);
        cfg->webUISessionCount = 0;
        const cJSON *session;
        cJSON_ArrayForEach(session, web_ui_sessions) {
            if (cfg->webUISessionCount >= CONFIG__WEBUI_MAX_SESSIONS) break;
            if (!cJSON_IsString(session) || session->valuestring == NULL) continue;
            cfg->webUISessions[cfg->webUISessionCount] = config__strdup(session->valuestring);
            ++cfg->webUISessionCount;
        }
    }

    const cJSON *wifi_ap = cJSON_GetObjectItemCaseSensitive(root, "wifiAp");
    if (cJSON_IsObject(wifi_ap)) {
        json_get_string(wifi_ap, "ssid", &cfg->wifiApSsid);
        json_get_string(wifi_ap, "pwd", &cfg->wifiApPassword);
    }
    json_get_string(root, "wifiMAC", &cfg->wifiMAC);
    json_get_bool(root, "TerminalLog", &cfg->terminalLog);

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        for (size_t i = 0; i < CONFIG__WIFI_MAX_CREDENTIALS; ++i) {
            config__release(&cfg->wifiCredentials[i].ssid);
            config__release(&cfg->wifiCredentials[i].password);
        }
        cfg->wifiCredentialCount = 0;
        const cJSON *entry;
        cJSON_ArrayForEach(entry, wifi) {
            if (cfg->wifiCredentialCount >= CONFIG__WIFI_MAX_CREDENTIALS) break;
            if (entry->string == NULL || !cJSON_IsString(entry) || entry->valuestring == NULL) continue;
            bruce_config_wifi_credential_t *credential = &cfg->wifiCredentials[cfg->wifiCredentialCount];
            credential->ssid = config__strdup(entry->string);
            credential->password = config__strdup(entry->valuestring);
            ++cfg->wifiCredentialCount;
        }
    }

    const cJSON *startup_apps = cJSON_GetObjectItemCaseSensitive(root, "startupApps");
    if (cJSON_IsArray(startup_apps)) {
        config__clear_string_array(cfg->startupApps.items, CONFIG__STARTUP_APP_MAX_COUNT);
        cfg->startupApps.count = 0;
        const cJSON *app;
        cJSON_ArrayForEach(app, startup_apps) {
            if (cfg->startupApps.count >= CONFIG__STARTUP_APP_MAX_COUNT) break;
            if (!cJSON_IsString(app) || app->valuestring == NULL) continue;
            if (!config__valid_value(app->valuestring, CONFIG__STARTUP_APP_MAX_LEN, false)) continue;
            cfg->startupApps.items[cfg->startupApps.count] = config__strdup(app->valuestring);
            ++cfg->startupApps.count;
        }
    }
    json_get_string(root, "startupAppJSInterpreterFile", &cfg->startupAppJSInterpreterFile);
    json_get_string(root, "wigleBasicToken", &cfg->wigleBasicToken);
    json_get_string(root, "wdgwarsApiKey", &cfg->wdgwarsApiKey);
    json_get_int(root, "devMode", &cfg->devMode);
    json_get_int(root, "colorInverted", &cfg->colorInverted);

    json_get_int(root, "badUSBBLEKeyboardLayout", &cfg->badUSBBLEKeyboardLayout);
    json_get_uint16(root, "badUSBBLEKeyDelay", &cfg->badUSBBLEKeyDelay);
    json_get_bool(root, "badUSBBLEShowOutput", &cfg->badUSBBLEShowOutput);

    const cJSON *disabled_menus = cJSON_GetObjectItemCaseSensitive(root, "disabledMenus");
    if (cJSON_IsArray(disabled_menus)) {
        config__clear_string_array(cfg->disabledMenus, CONFIG__DISABLED_MENU_MAX_COUNT);
        cfg->disabledMenuCount = 0;
        const cJSON *menu;
        cJSON_ArrayForEach(menu, disabled_menus) {
            if (cfg->disabledMenuCount >= CONFIG__DISABLED_MENU_MAX_COUNT) break;
            if (!cJSON_IsString(menu) || menu->valuestring == NULL) continue;
            cfg->disabledMenus[cfg->disabledMenuCount] = config__strdup(menu->valuestring);
            ++cfg->disabledMenuCount;
        }
    }
}

static cJSON *config__build_json(const config__t *cfg) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    char hex[16];
    snprintf(hex, sizeof(hex), "%x", cfg->priColor);
    cJSON_AddStringToObject(root, "priColor", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->secColor);
    cJSON_AddStringToObject(root, "secColor", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->bgColor);
    cJSON_AddStringToObject(root, "bgColor", hex);
    cJSON_AddStringToObject(root, "themeFile", config__or_empty(cfg->themePath));
    cJSON_AddBoolToObject(root, "themeOnSd", cfg->themeOnSd);
    cJSON_AddBoolToObject(root, "displayDmaFramebuffer", cfg->displayDmaFramebuffer);
    cJSON_AddStringToObject(root, "launcherApp", config__or_empty(cfg->launcherApp));

    cJSON_AddNumberToObject(root, "dimmerSet", cfg->dimmerSet);
    cJSON_AddNumberToObject(root, "bright", cfg->bright);
    cJSON_AddBoolToObject(root, "automaticTimeUpdateViaNTP", cfg->automaticTimeUpdateViaNTP);
    cJSON_AddNumberToObject(root, "tmz", cfg->tmz);
    cJSON_AddBoolToObject(root, "dst", cfg->dst);
    cJSON_AddBoolToObject(root, "clock24hr", cfg->clock24hr);
    cJSON_AddNumberToObject(root, "soundEnabled", cfg->soundEnabled);
    cJSON_AddNumberToObject(root, "soundVolume", cfg->soundVolume);
    cJSON_AddNumberToObject(root, "wifiAtStartup", cfg->wifiAtStartup);
    cJSON_AddNumberToObject(root, "instantBoot", cfg->instantBoot);
    cJSON_AddStringToObject(root, "keyboardLang", config__or_empty(cfg->keyboardLang));

    cJSON *hotkeys = cJSON_AddObjectToObject(root, "hotkeys");
    for (size_t i = 0; i < cfg->hotkeys.count; ++i) {
        cJSON_AddStringToObject(
            hotkeys,
            config__or_empty(cfg->hotkeys.items[i].key),
            config__or_empty(cfg->hotkeys.items[i].action)
        );
    }

    cJSON_AddNumberToObject(root, "ledBright", cfg->ledBright);
    snprintf(hex, sizeof(hex), "%lx", (unsigned long)cfg->ledColor);
    cJSON_AddStringToObject(root, "ledColor", hex);
    cJSON_AddNumberToObject(root, "ledBlinkEnabled", cfg->ledBlinkEnabled);
    cJSON_AddNumberToObject(root, "ledEffect", cfg->ledEffect);
    cJSON_AddNumberToObject(root, "ledEffectSpeed", cfg->ledEffectSpeed);
    cJSON_AddNumberToObject(root, "ledEffectDirection", cfg->ledEffectDirection);

    cJSON *web_ui = cJSON_AddObjectToObject(root, "webUI");
    cJSON_AddStringToObject(web_ui, "user", config__or_empty(cfg->webUIUser));
    cJSON_AddStringToObject(web_ui, "pwd", config__or_empty(cfg->webUIPassword));

    cJSON *web_ui_sessions = cJSON_AddObjectToObject(root, "webUISessions");
    for (size_t i = 0; i < cfg->webUISessionCount; ++i) {
        char key[12];
        snprintf(key, sizeof(key), "%u", (unsigned int)(i + 1));
        cJSON_AddStringToObject(web_ui_sessions, key, config__or_empty(cfg->webUISessions[i]));
    }

    cJSON *wifi_ap = cJSON_AddObjectToObject(root, "wifiAp");
    cJSON_AddStringToObject(wifi_ap, "ssid", config__or_empty(cfg->wifiApSsid));
    cJSON_AddStringToObject(wifi_ap, "pwd", config__or_empty(cfg->wifiApPassword));
    cJSON_AddStringToObject(root, "wifiMAC", config__or_empty(cfg->wifiMAC));
    cJSON_AddBoolToObject(root, "TerminalLog", cfg->terminalLog);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    for (size_t i = 0; i < cfg->wifiCredentialCount; ++i) {
        cJSON_AddStringToObject(
            wifi,
            config__or_empty(cfg->wifiCredentials[i].ssid),
            config__or_empty(cfg->wifiCredentials[i].password)
        );
    }

    cJSON *startup_apps = cJSON_AddArrayToObject(root, "startupApps");
    for (size_t i = 0; i < cfg->startupApps.count; ++i) {
        cJSON_AddItemToArray(startup_apps, cJSON_CreateString(config__or_empty(cfg->startupApps.items[i])));
    }
    cJSON_AddStringToObject(
        root, "startupAppJSInterpreterFile", config__or_empty(cfg->startupAppJSInterpreterFile)
    );
    cJSON_AddStringToObject(root, "wigleBasicToken", config__or_empty(cfg->wigleBasicToken));
    cJSON_AddStringToObject(root, "wdgwarsApiKey", config__or_empty(cfg->wdgwarsApiKey));
    cJSON_AddNumberToObject(root, "devMode", cfg->devMode);
    cJSON_AddNumberToObject(root, "colorInverted", cfg->colorInverted);

    cJSON_AddNumberToObject(root, "badUSBBLEKeyboardLayout", cfg->badUSBBLEKeyboardLayout);
    cJSON_AddNumberToObject(root, "badUSBBLEKeyDelay", cfg->badUSBBLEKeyDelay);
    cJSON_AddBoolToObject(root, "badUSBBLEShowOutput", cfg->badUSBBLEShowOutput);

    cJSON *disabled_menus = cJSON_AddArrayToObject(root, "disabledMenus");
    for (size_t i = 0; i < cfg->disabledMenuCount; ++i) {
        cJSON_AddItemToArray(disabled_menus, cJSON_CreateString(config__or_empty(cfg->disabledMenus[i])));
    }

    return root;
}

/* ------------------------------------------------------------------------ */
/* Load / save                                                               */
/* ------------------------------------------------------------------------ */

static bool config__save_locked(void) {
    cJSON *root = config__build_json(&s_config);
    if (root == NULL) return false;

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) return false;

    bool saved = storage__write_file_atomic(CONFIG__FILE_PATH, text, strlen(text));
    cJSON_free(text);
    return saved;
}

void config__init_defaults(void) {
    config__lock();
    if (!s_defaults_initialized) {
        config__set_defaults(&s_config);
        s_defaults_initialized = true;
    }
    config__unlock();
}

bool config__init(void) {
    config__ensure_mutex();
    config__init_defaults();
    return config__load();
}

bool config__load(void) {
    config__init_defaults();
    config__lock();
    if (s_loaded) {
        config__unlock();
        return true;
    }
    char *text = NULL;
    size_t size = 0;
    bool read = storage__read_file(CONFIG__FILE_PATH, &text, &size);
    if (read && size > 0) {
        cJSON *root = cJSON_ParseWithLength(text, size);
        storage__free(text);
        text = NULL;
        if (root != NULL) {
            config__parse_json(&s_config, root);
            cJSON_Delete(root);
        }
    }
    if (text != NULL) storage__free(text);
    config__validate(&s_config);
    s_loaded = true;
    bool result = read || config__save_locked();
    config__unlock();
    return result;
}

bool config__save(void) {
    if (!config__init()) return false;
    config__lock();
    config__validate(&s_config);
    bool saved = config__save_locked();
    config__unlock();
    return saved;
}

bool config__factory_reset(void) {
    if (!config__init()) return false;
    config__lock();
    config__set_defaults(&s_config);
    bool saved = config__save_locked();
    config__unlock();
    return saved;
}

/* ------------------------------------------------------------------------ */
/* Public Config API (core_sdk/config.h)                                    */
/* ------------------------------------------------------------------------ */

/* True for a built-in caller or when there is no current Core process at all
 * (e.g. boot), mirroring permission__check()'s own implicit-grant rule.
 * Used by the permanently-protected fields, which bypass `config`
 * permission checks entirely for such callers but are never accessible to
 * an external process, no matter what it has been granted. */
static bool config__caller_is_trusted(void) {
    bool built_in = false;
    bruce_result_t context = process_registry__current_context(&built_in, NULL, 0, NULL);
    return context == BRUCE_ERR_NOT_FOUND || built_in;
}

static bruce_result_t config__guard(void) { return permission__check(BRUCE_PERMISSION_CONFIG); }

static bruce_result_t config__guard_protected(void) {
    return config__caller_is_trusted() ? BRUCE_OK : BRUCE_ERR_PERMISSION;
}

typedef bruce_result_t (*config__guard_fn_t)(void);

#define CONFIG__DEFINE_STRING_GETTER_GUARDED(NAME, FIELD, GUARD_FN)                                          \
    const char *config__get_##NAME(void) {                                                                   \
        if ((GUARD_FN)() != BRUCE_OK || !config__init()) return NULL;                                        \
        config__lock();                                                                                      \
        const char *value = s_config.FIELD;                                                                  \
        config__unlock();                                                                                    \
        return value;                                                                                        \
    }

#define CONFIG__DEFINE_STRING_FIELD_GUARDED(NAME, FIELD, MAX_LEN, GUARD_FN)                                  \
    CONFIG__DEFINE_STRING_GETTER_GUARDED(NAME, FIELD, GUARD_FN)                                              \
    bruce_result_t config__set_##NAME(const char *value) {                                                   \
        bruce_result_t guard = (GUARD_FN)();                                                                 \
        if (guard != BRUCE_OK) return guard;                                                                 \
        if (!config__init() || !config__valid_value(value, (MAX_LEN), true))                                 \
            return BRUCE_ERR_INVALID_ARGUMENT;                                                               \
        config__lock();                                                                                      \
        config__assign(&s_config.FIELD, value);                                                              \
        config__validate(&s_config);                                                                         \
        bool saved = config__save_locked();                                                                  \
        config__unlock();                                                                                    \
        return saved ? BRUCE_OK : BRUCE_ERR_IO;                                                              \
    }
#define CONFIG__DEFINE_STRING_FIELD(NAME, FIELD, MAX_LEN)                                                    \
    CONFIG__DEFINE_STRING_FIELD_GUARDED(NAME, FIELD, MAX_LEN, config__guard)

#define CONFIG__DEFINE_INT_FIELD_GUARDED(NAME, FIELD, GUARD_FN)                                              \
    int config__get_##NAME(void) {                                                                           \
        if ((GUARD_FN)() != BRUCE_OK || !config__init()) return 0;                                           \
        config__lock();                                                                                      \
        int value = s_config.FIELD;                                                                          \
        config__unlock();                                                                                    \
        return value;                                                                                        \
    }                                                                                                        \
    bruce_result_t config__set_##NAME(int value) {                                                           \
        bruce_result_t guard = (GUARD_FN)();                                                                 \
        if (guard != BRUCE_OK) return guard;                                                                 \
        if (!config__init()) return BRUCE_ERR_IO;                                                            \
        config__lock();                                                                                      \
        s_config.FIELD = value;                                                                              \
        config__validate(&s_config);                                                                         \
        bool saved = config__save_locked();                                                                  \
        config__unlock();                                                                                    \
        return saved ? BRUCE_OK : BRUCE_ERR_IO;                                                              \
    }
#define CONFIG__DEFINE_INT_FIELD(NAME, FIELD) CONFIG__DEFINE_INT_FIELD_GUARDED(NAME, FIELD, config__guard)

#define CONFIG__DEFINE_BOOL_FIELD_GUARDED(NAME, FIELD, GUARD_FN)                                             \
    bool config__get_##NAME(void) {                                                                          \
        if ((GUARD_FN)() != BRUCE_OK || !config__init()) return false;                                       \
        config__lock();                                                                                      \
        bool value = s_config.FIELD;                                                                         \
        config__unlock();                                                                                    \
        return value;                                                                                        \
    }                                                                                                        \
    bruce_result_t config__set_##NAME(bool value) {                                                          \
        bruce_result_t guard = (GUARD_FN)();                                                                 \
        if (guard != BRUCE_OK) return guard;                                                                 \
        if (!config__init()) return BRUCE_ERR_IO;                                                            \
        config__lock();                                                                                      \
        s_config.FIELD = value;                                                                              \
        config__validate(&s_config);                                                                         \
        bool saved = config__save_locked();                                                                  \
        config__unlock();                                                                                    \
        return saved ? BRUCE_OK : BRUCE_ERR_IO;                                                              \
    }
#define CONFIG__DEFINE_BOOL_FIELD(NAME, FIELD) CONFIG__DEFINE_BOOL_FIELD_GUARDED(NAME, FIELD, config__guard)

/* Same as CONFIG__DEFINE_BOOL_FIELD, but FIELD is an `int` (historically
 * BrucePIO stored these as 0/1 ints, not C bool). */
#define CONFIG__DEFINE_BOOL_INT_FIELD_GUARDED(NAME, FIELD, GUARD_FN)                                         \
    bool config__get_##NAME(void) {                                                                          \
        if ((GUARD_FN)() != BRUCE_OK || !config__init()) return false;                                       \
        config__lock();                                                                                      \
        bool value = s_config.FIELD != 0;                                                                    \
        config__unlock();                                                                                    \
        return value;                                                                                        \
    }                                                                                                        \
    bruce_result_t config__set_##NAME(bool value) {                                                          \
        bruce_result_t guard = (GUARD_FN)();                                                                 \
        if (guard != BRUCE_OK) return guard;                                                                 \
        if (!config__init()) return BRUCE_ERR_IO;                                                            \
        config__lock();                                                                                      \
        s_config.FIELD = value ? 1 : 0;                                                                      \
        config__validate(&s_config);                                                                         \
        bool saved = config__save_locked();                                                                  \
        config__unlock();                                                                                    \
        return saved ? BRUCE_OK : BRUCE_ERR_IO;                                                              \
    }
#define CONFIG__DEFINE_BOOL_INT_FIELD(NAME, FIELD)                                                           \
    CONFIG__DEFINE_BOOL_INT_FIELD_GUARDED(NAME, FIELD, config__guard)

#define CONFIG__DEFINE_FLOAT_FIELD_GUARDED(NAME, FIELD, GUARD_FN)                                            \
    float config__get_##NAME(void) {                                                                         \
        if ((GUARD_FN)() != BRUCE_OK || !config__init()) return 0;                                           \
        config__lock();                                                                                      \
        float value = s_config.FIELD;                                                                        \
        config__unlock();                                                                                    \
        return value;                                                                                        \
    }                                                                                                        \
    bruce_result_t config__set_##NAME(float value) {                                                         \
        bruce_result_t guard = (GUARD_FN)();                                                                 \
        if (guard != BRUCE_OK) return guard;                                                                 \
        if (!config__init()) return BRUCE_ERR_IO;                                                            \
        config__lock();                                                                                      \
        s_config.FIELD = value;                                                                              \
        config__validate(&s_config);                                                                         \
        bool saved = config__save_locked();                                                                  \
        config__unlock();                                                                                    \
        return saved ? BRUCE_OK : BRUCE_ERR_IO;                                                              \
    }
#define CONFIG__DEFINE_FLOAT_FIELD(NAME, FIELD) CONFIG__DEFINE_FLOAT_FIELD_GUARDED(NAME, FIELD, config__guard)

#define CONFIG__DEFINE_UINT16_FIELD_GUARDED(NAME, FIELD, GUARD_FN)                                           \
    uint16_t config__get_##NAME(void) {                                                                      \
        if ((GUARD_FN)() != BRUCE_OK || !config__init()) return 0;                                           \
        config__lock();                                                                                      \
        uint16_t value = s_config.FIELD;                                                                     \
        config__unlock();                                                                                    \
        return value;                                                                                        \
    }                                                                                                        \
    bruce_result_t config__set_##NAME(uint16_t value) {                                                      \
        bruce_result_t guard = (GUARD_FN)();                                                                 \
        if (guard != BRUCE_OK) return guard;                                                                 \
        if (!config__init()) return BRUCE_ERR_IO;                                                            \
        config__lock();                                                                                      \
        s_config.FIELD = value;                                                                              \
        config__validate(&s_config);                                                                         \
        bool saved = config__save_locked();                                                                  \
        config__unlock();                                                                                    \
        return saved ? BRUCE_OK : BRUCE_ERR_IO;                                                              \
    }
#define CONFIG__DEFINE_UINT16_FIELD(NAME, FIELD)                                                             \
    CONFIG__DEFINE_UINT16_FIELD_GUARDED(NAME, FIELD, config__guard)

#define CONFIG__DEFINE_UINT32_FIELD_GUARDED(NAME, FIELD, GUARD_FN)                                           \
    uint32_t config__get_##NAME(void) {                                                                      \
        if ((GUARD_FN)() != BRUCE_OK || !config__init()) return 0;                                           \
        config__lock();                                                                                      \
        uint32_t value = s_config.FIELD;                                                                     \
        config__unlock();                                                                                    \
        return value;                                                                                        \
    }                                                                                                        \
    bruce_result_t config__set_##NAME(uint32_t value) {                                                      \
        bruce_result_t guard = (GUARD_FN)();                                                                 \
        if (guard != BRUCE_OK) return guard;                                                                 \
        if (!config__init()) return BRUCE_ERR_IO;                                                            \
        config__lock();                                                                                      \
        s_config.FIELD = value;                                                                              \
        config__validate(&s_config);                                                                         \
        bool saved = config__save_locked();                                                                  \
        config__unlock();                                                                                    \
        return saved ? BRUCE_OK : BRUCE_ERR_IO;                                                              \
    }
#define CONFIG__DEFINE_UINT32_FIELD(NAME, FIELD)                                                             \
    CONFIG__DEFINE_UINT32_FIELD_GUARDED(NAME, FIELD, config__guard)

/* ---- Permanently protected (ELF/JS never; built-ins/no-context bypass) --- */

CONFIG__DEFINE_STRING_GETTER_GUARDED(wifi_ap_ssid, wifiApSsid, config__guard_protected)
CONFIG__DEFINE_STRING_GETTER_GUARDED(wifi_ap_password, wifiApPassword, config__guard_protected)

bruce_result_t config__set_wifi_ap(const char *ssid, const char *password) {
    bruce_result_t guard = config__guard_protected();
    if (guard != BRUCE_OK) return guard;
    if (!config__init() || !config__valid_value(ssid, CONFIG__WIFI_SSID_MAX_LEN, false) ||
        !config__valid_value(password, CONFIG__WIFI_PASSWORD_MAX_LEN, false)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    config__lock();
    config__assign(&s_config.wifiApSsid, ssid);
    config__assign(&s_config.wifiApPassword, password);
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

size_t config__wifi_credential_count(void) {
    if (config__guard_protected() != BRUCE_OK || !config__init()) return 0;
    config__lock();
    size_t count = s_config.wifiCredentialCount;
    config__unlock();
    return count;
}

const bruce_config_wifi_credential_t *config__wifi_credential_at(size_t index) {
    if (config__guard_protected() != BRUCE_OK || !config__init()) return NULL;
    config__lock();
    const bruce_config_wifi_credential_t *credential =
        index < s_config.wifiCredentialCount ? &s_config.wifiCredentials[index] : NULL;
    config__unlock();
    return credential;
}

const bruce_config_wifi_credential_t *config__find_wifi_credential(const char *ssid) {
    if (config__guard_protected() != BRUCE_OK || !config__init() || ssid == NULL) return NULL;
    config__lock();
    const bruce_config_wifi_credential_t *credential = NULL;
    for (size_t i = 0; i < s_config.wifiCredentialCount; ++i) {
        if (strcmp(s_config.wifiCredentials[i].ssid, ssid) == 0) {
            credential = &s_config.wifiCredentials[i];
            break;
        }
    }
    config__unlock();
    return credential;
}

bruce_result_t config__add_or_update_wifi_credential(const char *ssid, const char *password) {
    bruce_result_t guard = config__guard_protected();
    if (guard != BRUCE_OK) return guard;
    if (!config__init() || !config__valid_value(ssid, CONFIG__WIFI_SSID_MAX_LEN, false) ||
        !config__valid_value(password, CONFIG__WIFI_PASSWORD_MAX_LEN, true)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    config__lock();
    for (size_t i = 0; i < s_config.wifiCredentialCount; ++i) {
        if (strcmp(s_config.wifiCredentials[i].ssid, ssid) == 0) {
            config__assign(&s_config.wifiCredentials[i].password, password);
            bool saved = config__save_locked();
            config__unlock();
            return saved ? BRUCE_OK : BRUCE_ERR_IO;
        }
    }
    if (s_config.wifiCredentialCount == CONFIG__WIFI_MAX_CREDENTIALS) {
        config__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    bruce_config_wifi_credential_t *slot = &s_config.wifiCredentials[s_config.wifiCredentialCount];
    slot->ssid = config__strdup(ssid);
    slot->password = config__strdup(password);
    ++s_config.wifiCredentialCount;
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

CONFIG__DEFINE_STRING_FIELD_GUARDED(wifi_mac, wifiMAC, CONFIG__WIFI_MAC_MAX_LEN, config__guard_protected)
CONFIG__DEFINE_STRING_FIELD_GUARDED(
    web_ui_user, webUIUser, CONFIG__WEBUI_USER_MAX_LEN, config__guard_protected
)
CONFIG__DEFINE_STRING_FIELD_GUARDED(
    web_ui_password, webUIPassword, CONFIG__WEBUI_PASSWORD_MAX_LEN, config__guard_protected
)

/* ---- `config`-permission-gated fields ------------------------------------ */

CONFIG__DEFINE_UINT16_FIELD(pri_color, priColor)
CONFIG__DEFINE_UINT16_FIELD(sec_color, secColor)
CONFIG__DEFINE_UINT16_FIELD(bg_color, bgColor)
CONFIG__DEFINE_STRING_FIELD(theme_path, themePath, CONFIG__THEME_PATH_MAX_LEN)
CONFIG__DEFINE_BOOL_FIELD(theme_on_sd, themeOnSd)
CONFIG__DEFINE_BOOL_FIELD(display_dma_framebuffer, displayDmaFramebuffer)
CONFIG__DEFINE_STRING_FIELD(launcher_app, launcherApp, CONFIG__LAUNCHER_APP_MAX_LEN)

CONFIG__DEFINE_INT_FIELD(dimmer_set, dimmerSet)
CONFIG__DEFINE_INT_FIELD(bright, bright)
CONFIG__DEFINE_BOOL_FIELD(automatic_time_update_via_ntp, automaticTimeUpdateViaNTP)
CONFIG__DEFINE_FLOAT_FIELD(tmz, tmz)
CONFIG__DEFINE_BOOL_FIELD(dst, dst)
CONFIG__DEFINE_BOOL_FIELD(clock24hr, clock24hr)
CONFIG__DEFINE_BOOL_INT_FIELD(sound_enabled, soundEnabled)
CONFIG__DEFINE_INT_FIELD(sound_volume, soundVolume)
CONFIG__DEFINE_BOOL_INT_FIELD(wifi_at_startup, wifiAtStartup)
CONFIG__DEFINE_BOOL_INT_FIELD(instant_boot, instantBoot)
CONFIG__DEFINE_STRING_FIELD(keyboard_lang, keyboardLang, CONFIG__KEYBOARD_LANG_MAX_LEN)

const bruce_config_hotkeys_t *config__get_hotkeys(void) {
    if (config__guard() != BRUCE_OK || !config__init()) return NULL;
    config__lock();
    const bruce_config_hotkeys_t *hotkeys = &s_config.hotkeys;
    config__unlock();
    return hotkeys;
}

bruce_result_t config__set_hotkeys(const bruce_config_hotkey_t *values, size_t count) {
    bruce_result_t guard = config__guard();
    if (guard != BRUCE_OK) return guard;
    if (!config__init() || count > CONFIG__HOTKEY_MAX_COUNT || (count > 0 && values == NULL)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_config_hotkey_t copies[CONFIG__HOTKEY_MAX_COUNT] = {0};
    for (size_t i = 0; i < count; ++i) {
        if (!config__valid_value(values[i].key, CONFIG__HOTKEY_MAX_LEN, false) ||
            !config__valid_value(values[i].action, CONFIG__HOTKEY_ACTION_MAX_LEN, false)) {
            for (size_t j = 0; j < i; ++j) {
                config__release(&copies[j].key);
                config__release(&copies[j].action);
            }
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        copies[i].key = config__strdup(values[i].key);
        copies[i].action = config__strdup(values[i].action);
        if (copies[i].key == NULL || copies[i].action == NULL) {
            for (size_t j = 0; j <= i; ++j) {
                config__release(&copies[j].key);
                config__release(&copies[j].action);
            }
            return BRUCE_ERR_NO_MEMORY;
        }
    }

    config__lock();
    for (size_t i = 0; i < CONFIG__HOTKEY_MAX_COUNT; ++i) {
        config__release(&s_config.hotkeys.items[i].key);
        config__release(&s_config.hotkeys.items[i].action);
    }
    memcpy(s_config.hotkeys.items, copies, sizeof(copies));
    s_config.hotkeys.count = count;
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

CONFIG__DEFINE_INT_FIELD(led_bright, ledBright)
CONFIG__DEFINE_UINT32_FIELD(led_color, ledColor)
CONFIG__DEFINE_BOOL_INT_FIELD(led_blink_enabled, ledBlinkEnabled)
CONFIG__DEFINE_INT_FIELD(led_effect, ledEffect)
CONFIG__DEFINE_INT_FIELD(led_effect_speed, ledEffectSpeed)
CONFIG__DEFINE_INT_FIELD(led_effect_direction, ledEffectDirection)

const bruce_config_startup_apps_t *config__get_startup_apps(void) {
    if (config__guard() != BRUCE_OK || !config__init()) return NULL;
    config__lock();
    const bruce_config_startup_apps_t *apps = &s_config.startupApps;
    config__unlock();
    return apps;
}

bruce_result_t config__set_startup_apps(const char *const *values, size_t count) {
    bruce_result_t guard = config__guard();
    if (guard != BRUCE_OK) return guard;
    if (!config__init() || count > CONFIG__STARTUP_APP_MAX_COUNT || (count > 0 && values == NULL)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    const char *copies[CONFIG__STARTUP_APP_MAX_COUNT] = {0};
    for (size_t i = 0; i < count; ++i) {
        if (!config__valid_value(values[i], CONFIG__STARTUP_APP_MAX_LEN, false)) {
            config__clear_string_array(copies, CONFIG__STARTUP_APP_MAX_COUNT);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        copies[i] = config__strdup(values[i]);
        if (copies[i] == NULL) {
            config__clear_string_array(copies, CONFIG__STARTUP_APP_MAX_COUNT);
            return BRUCE_ERR_NO_MEMORY;
        }
    }

    config__lock();
    config__clear_string_array(s_config.startupApps.items, CONFIG__STARTUP_APP_MAX_COUNT);
    memcpy(s_config.startupApps.items, copies, sizeof(copies));
    s_config.startupApps.count = count;
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t config__add_startup_app(const char *key) {
    bruce_result_t guard = config__guard();
    if (guard != BRUCE_OK) return guard;
    if (!config__init() || !config__valid_value(key, CONFIG__STARTUP_APP_MAX_LEN, false)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    config__lock();
    for (size_t i = 0; i < s_config.startupApps.count; ++i) {
        if (strcmp(s_config.startupApps.items[i], key) == 0) {
            config__unlock();
            return BRUCE_OK;
        }
    }
    if (s_config.startupApps.count == CONFIG__STARTUP_APP_MAX_COUNT) {
        config__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    char *copy = config__strdup(key);
    if (copy == NULL) {
        config__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    s_config.startupApps.items[s_config.startupApps.count++] = copy;
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t config__remove_startup_app(const char *key) {
    bruce_result_t guard = config__guard();
    if (guard != BRUCE_OK) return guard;
    if (!config__init() || !config__valid_value(key, CONFIG__STARTUP_APP_MAX_LEN, false)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    config__lock();
    size_t index = 0;
    while (index < s_config.startupApps.count && strcmp(s_config.startupApps.items[index], key) != 0) {
        ++index;
    }
    if (index == s_config.startupApps.count) {
        config__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }

    config__release(&s_config.startupApps.items[index]);
    --s_config.startupApps.count;
    memmove(
        &s_config.startupApps.items[index],
        &s_config.startupApps.items[index + 1],
        (s_config.startupApps.count - index) * sizeof(s_config.startupApps.items[0])
    );
    s_config.startupApps.items[s_config.startupApps.count] = NULL;
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

CONFIG__DEFINE_STRING_GETTER_GUARDED(
    startup_app_js_interpreter_file, startupAppJSInterpreterFile, config__guard
)
CONFIG__DEFINE_STRING_GETTER_GUARDED(wigle_basic_token, wigleBasicToken, config__guard)
CONFIG__DEFINE_STRING_GETTER_GUARDED(wdgwars_api_key, wdgwarsApiKey, config__guard)
CONFIG__DEFINE_BOOL_INT_FIELD(dev_mode, devMode)
CONFIG__DEFINE_BOOL_INT_FIELD(color_inverted, colorInverted)

/* ------------------------------------------------------------------------ */
/* List helpers                                                              */
/* ------------------------------------------------------------------------ */

bool config__add_disabled_menu(const char *value) {
    if (!config__init() || !config__valid_value(value, CONFIG__DISABLED_MENU_MAX_LEN, false)) return false;
    config__lock();
    if (s_config.disabledMenuCount == CONFIG__DISABLED_MENU_MAX_COUNT) {
        config__unlock();
        return false;
    }
    s_config.disabledMenus[s_config.disabledMenuCount] = config__strdup(value);
    ++s_config.disabledMenuCount;
    bool saved = config__save_locked();
    config__unlock();
    return saved;
}

bool config__add_web_ui_session(const char *token) {
    if (!config__init() || !config__valid_value(token, CONFIG__WEBUI_SESSION_TOKEN_MAX_LEN, false))
        return false;
    config__lock();
    if (s_config.webUISessionCount == CONFIG__WEBUI_MAX_SESSIONS) {
        /* FIFO eviction: drop the oldest session, matching BruceConfig::addWebUISession. */
        config__release(&s_config.webUISessions[0]);
        memmove(
            &s_config.webUISessions[0],
            &s_config.webUISessions[1],
            (CONFIG__WEBUI_MAX_SESSIONS - 1) * sizeof(s_config.webUISessions[0])
        );
        --s_config.webUISessionCount;
    }
    s_config.webUISessions[s_config.webUISessionCount] = config__strdup(token);
    ++s_config.webUISessionCount;
    bool saved = config__save_locked();
    config__unlock();
    return saved;
}

bruce_result_t config__create_web_ui_session(char *out_token, size_t capacity) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (config__guard_protected() != BRUCE_OK) return BRUCE_ERR_PERMISSION;
    if (out_token == NULL || capacity < BRUCE_CONFIG_WEB_UI_SESSION_TOKEN_LEN + 1u) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < BRUCE_CONFIG_WEB_UI_SESSION_TOKEN_LEN; ++i) {
        out_token[i] = alphabet[esp_random() % (sizeof(alphabet) - 1u)];
    }
    out_token[BRUCE_CONFIG_WEB_UI_SESSION_TOKEN_LEN] = '\0';
    return config__add_web_ui_session(out_token) ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t config__remove_web_ui_session(const char *token) {
    if (config__guard_protected() != BRUCE_OK) return BRUCE_ERR_PERMISSION;
    if (!config__init() || token == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    config__lock();
    size_t write_index = 0;
    bool removed = false;
    for (size_t read_index = 0; read_index < s_config.webUISessionCount; ++read_index) {
        if (strcmp(s_config.webUISessions[read_index], token) == 0) {
            config__release(&s_config.webUISessions[read_index]);
            removed = true;
            continue;
        }
        if (write_index != read_index) {
            s_config.webUISessions[write_index] = s_config.webUISessions[read_index];
            s_config.webUISessions[read_index] = NULL;
        }
        ++write_index;
    }
    s_config.webUISessionCount = write_index;
    bool saved = config__save_locked();
    config__unlock();
    if (!removed) return BRUCE_ERR_NOT_FOUND;
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

bool config__is_valid_web_ui_session(const char *token) {
    if (config__guard_protected() != BRUCE_OK || !config__init() || token == NULL) return false;
    config__lock();
    bool found = false;
    for (size_t i = 0; i < s_config.webUISessionCount; ++i) {
        if (strcmp(s_config.webUISessions[i], token) == 0) {
            found = true;
            break;
        }
    }
    config__unlock();
    return found;
}
