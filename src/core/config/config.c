#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/process/process.h"
#include "core/storage/storage.h"
#include "core_sdk/config.h"
#include "core_sdk/permission.h"
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
    config__release(&cfg->launcher);
    config__release(&cfg->keyboardLang);
    for (size_t i = 0; i < CONFIG__HOTKEY_MAX_COUNT; ++i) {
        config__release(&cfg->hotkeys.items[i].key);
        config__release(&cfg->hotkeys.items[i].action);
    }
    config__release(&cfg->wifiApSsid);
    config__release(&cfg->wifiApPassword);
    for (size_t i = 0; i < CONFIG__WIFI_MAX_CREDENTIALS; ++i) {
        config__release(&cfg->wifiCredentials[i].ssid);
        config__release(&cfg->wifiCredentials[i].password);
    }
    config__release(&cfg->wifiMac);
    for (size_t i = 0; i < CONFIG__STARTUP_APP_MAX_COUNT; ++i) config__release(&cfg->startup.items[i]);
}

static void config__set_defaults(config__t *cfg) {
    config__free_config(cfg);
    memset(cfg, 0, sizeof(*cfg));

    cfg->colorPrimary = 0xA80F;
    cfg->colorSecondary = 0xA80F - 0x2000;
    cfg->colorBackground = 0x0000;
    cfg->colorSurface = 0x1082;
    cfg->colorText = 0xFFFF;
    cfg->colorTextMuted = 0x8410;
    cfg->colorBorder = 0x4208;
    cfg->colorSuccess = 0x07E0;
    cfg->colorWarning = 0xFD20;
    cfg->colorError = 0xF800;
    cfg->displayRotation = 0;
    cfg->displayBufferedRendering = true;
    cfg->displayDmaFramebuffer = true;
    config__assign(&cfg->launcher, "");

    cfg->displayDimTimeout = 60;
    cfg->displayBrightness = 100;
    cfg->timeAutomaticUpdateViaNTP = true;
    cfg->timeTimezone = 0;
    cfg->timeDst = false;
    cfg->timeClock24hr = true;
    cfg->soundEnabled = true;
    cfg->soundVolume = 100;
    config__assign(&cfg->keyboardLang, "QWERTY");
    config__assign(&cfg->hotkeys.items[0].key, "alt + tab");
    config__assign(&cfg->hotkeys.items[0].action, "process switch next");
    config__assign(&cfg->hotkeys.items[1].key, "ctrl + tab");
    config__assign(&cfg->hotkeys.items[1].action, "process preview");
    config__assign(&cfg->hotkeys.items[2].key, "ctrl + space");
    config__assign(&cfg->hotkeys.items[2].action, "launcher");
    config__assign(&cfg->hotkeys.items[3].key, "BTN_A");
    config__assign(&cfg->hotkeys.items[3].action, "emit PREV");
    config__assign(&cfg->hotkeys.items[4].key, "BTN_B");
    config__assign(&cfg->hotkeys.items[4].action, "emit SELECT");
    config__assign(&cfg->hotkeys.items[5].key, "BTN_C");
    config__assign(&cfg->hotkeys.items[5].action, "emit NEXT");

#if CONFIG_BRUCE_BUTTON_SELECT_ENABLED && !CONFIG_BRUCE_BUTTON_A_ENABLED && !CONFIG_BRUCE_BUTTON_B_ENABLED && \
    !CONFIG_BRUCE_BUTTON_C_ENABLED && !CONFIG_BRUCE_BUTTON_D_ENABLED
    config__assign(&cfg->hotkeys.items[6].key, "500ms SELECT");
    config__assign(&cfg->hotkeys.items[6].action, "GUI=1 OVERLAY=1 menu");
#else
    config__assign(&cfg->hotkeys.items[6].key, "500ms BTN_A");
    config__assign(&cfg->hotkeys.items[6].action, "GUI=1 OVERLAY=1 menu");
#endif

    cfg->hotkeys.count = 7;

    cfg->ledBrightness = 50;
    cfg->ledEnabled = true;
    cfg->ledColor = 0x960064;
    cfg->ledBlinkEnabled = 1;
    cfg->ledEffect = 0;
    cfg->ledEffectSpeed = 5;
    cfg->ledEffectDirection = 1;

    config__assign(&cfg->wifiApSsid, "BruceNet");
    config__assign(&cfg->wifiApPassword, "brucenet");
    config__assign(&cfg->wifiMac, "");

    static const char *const default_startup_apps[] = {
        "device_bus",
        "input",
        "notification_service",
        "BG=0 bootanimation",
        "launcher -s",
        "serial_commands",
    };
    for (size_t i = 0; i < sizeof(default_startup_apps) / sizeof(default_startup_apps[0]); ++i) {
        config__assign(&cfg->startup.items[i], default_startup_apps[i]);
    }
    cfg->startup.count = sizeof(default_startup_apps) / sizeof(default_startup_apps[0]);
    cfg->devMode = 0;
}

/* ------------------------------------------------------------------------ */
/* Validation                                                                */
/* ------------------------------------------------------------------------ */

static void config__validate(config__t *cfg) {
    if (cfg->displayRotation < 0 || cfg->displayRotation > 3) cfg->displayRotation = 0;

    if (cfg->displayDimTimeout < 0) cfg->displayDimTimeout = 10;
    if (cfg->displayDimTimeout > 60) cfg->displayDimTimeout = 0;

    if (cfg->displayBrightness > 100) cfg->displayBrightness = 100;

    if (cfg->timeTimezone < -12 || cfg->timeTimezone > 14) cfg->timeTimezone = 0;

    if (cfg->soundVolume > 100) cfg->soundVolume = 100;

    if (cfg->ledBrightness < 0) cfg->ledBrightness = 0;
    if (cfg->ledBrightness > 100) cfg->ledBrightness = 100;
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

static void json_get_float(const cJSON *object, const char *key, float *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) *out = (float)item->valuedouble;
}

static void json_get_bool(const cJSON *object, const char *key, bool *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsBool(item)) *out = cJSON_IsTrue(item);
}

static int config__hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* See core_sdk/config.h for the accepted forms. */
bool config__parse_theme_color(const char *text, uint16_t *out_rgb565) {
    if (text == NULL || out_rgb565 == NULL || text[0] == '\0') return false;
    if (text[0] == '#') text++;
    size_t length = strlen(text);
    for (size_t i = 0; i < length; ++i) {
        if (config__hex_nibble(text[i]) < 0) return false;
    }
    if (length == 3) {
        /* CSS shorthand "RGB": each nibble doubled into a byte (0xA -> 0xAA). */
        unsigned r = (unsigned)config__hex_nibble(text[0]) * 0x11u;
        unsigned g = (unsigned)config__hex_nibble(text[1]) * 0x11u;
        unsigned b = (unsigned)config__hex_nibble(text[2]) * 0x11u;
        *out_rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        return true;
    }
    if (length == 6) {
        /* 24-bit "RRGGBB" downsampled to RGB565. */
        unsigned long value = strtoul(text, NULL, 16);
        unsigned r = (unsigned)((value >> 16) & 0xFFu);
        unsigned g = (unsigned)((value >> 8) & 0xFFu);
        unsigned b = (unsigned)(value & 0xFFu);
        *out_rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        return true;
    }
    if (length == 4) {
        /* Already native RGB565. */
        *out_rgb565 = (uint16_t)strtoul(text, NULL, 16);
        return true;
    }
    return false;
}

static void json_get_hex16(const cJSON *object, const char *key, uint16_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) config__parse_theme_color(item->valuestring, out);
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

    const cJSON *theme = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (cJSON_IsObject(theme)) {
        json_get_hex16(theme, "primary", &cfg->colorPrimary);
        json_get_hex16(theme, "secondary", &cfg->colorSecondary);
        json_get_hex16(theme, "background", &cfg->colorBackground);
        json_get_hex16(theme, "surface", &cfg->colorSurface);
        json_get_hex16(theme, "text", &cfg->colorText);
        json_get_hex16(theme, "textMuted", &cfg->colorTextMuted);
        json_get_hex16(theme, "border", &cfg->colorBorder);
        json_get_hex16(theme, "success", &cfg->colorSuccess);
        json_get_hex16(theme, "warning", &cfg->colorWarning);
        json_get_hex16(theme, "error", &cfg->colorError);
    }

    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (cJSON_IsObject(display)) {
        json_get_int(display, "rotation", &cfg->displayRotation);
        json_get_int(display, "dimTimeout", &cfg->displayDimTimeout);
        json_get_int(display, "brightness", &cfg->displayBrightness);
        json_get_bool(display, "bufferedRendering", &cfg->displayBufferedRendering);
        json_get_bool(display, "dmaFramebuffer", &cfg->displayDmaFramebuffer);
    }

    const cJSON *time = cJSON_GetObjectItemCaseSensitive(root, "time");
    if (cJSON_IsObject(time)) {
        json_get_bool(time, "automaticUpdateViaNTP", &cfg->timeAutomaticUpdateViaNTP);
        json_get_float(time, "timezone", &cfg->timeTimezone);
        json_get_bool(time, "dst", &cfg->timeDst);
        json_get_bool(time, "clock24hr", &cfg->timeClock24hr);
    }

    const cJSON *sound = cJSON_GetObjectItemCaseSensitive(root, "sound");
    if (cJSON_IsObject(sound)) {
        json_get_bool(sound, "enabled", &cfg->soundEnabled);
        json_get_int(sound, "volume", &cfg->soundVolume);
    }

    json_get_string(root, "keyboardLang", &cfg->keyboardLang);
    json_get_string(root, "launcher", &cfg->launcher);

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

    const cJSON *led = cJSON_GetObjectItemCaseSensitive(root, "led");
    if (cJSON_IsObject(led)) {
        json_get_bool(led, "enabled", &cfg->ledEnabled);
        json_get_int(led, "bright", &cfg->ledBrightness);
        json_get_hex32(led, "color", &cfg->ledColor);
        json_get_int(led, "blinkEnabled", &cfg->ledBlinkEnabled);
        json_get_int(led, "effect", &cfg->ledEffect);
        json_get_int(led, "effectSpeed", &cfg->ledEffectSpeed);
        json_get_int(led, "effectDirection", &cfg->ledEffectDirection);
    }

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    const cJSON *wifi_ap = cJSON_IsObject(wifi) ? cJSON_GetObjectItemCaseSensitive(wifi, "ap") : NULL;
    if (cJSON_IsObject(wifi_ap)) {
        json_get_string(wifi_ap, "ssid", &cfg->wifiApSsid);
        json_get_string(wifi_ap, "pwd", &cfg->wifiApPassword);
    }
    if (cJSON_IsObject(wifi)) json_get_string(wifi, "mac", &cfg->wifiMac);

    const cJSON *wifi_credentials = cJSON_IsObject(wifi) ? cJSON_GetObjectItemCaseSensitive(wifi, "credentials") : NULL;
    if (cJSON_IsObject(wifi_credentials)) {
        for (size_t i = 0; i < CONFIG__WIFI_MAX_CREDENTIALS; ++i) {
            config__release(&cfg->wifiCredentials[i].ssid);
            config__release(&cfg->wifiCredentials[i].password);
        }
        cfg->wifiCredentialCount = 0;
        const cJSON *entry;
        cJSON_ArrayForEach(entry, wifi_credentials) {
            if (cfg->wifiCredentialCount >= CONFIG__WIFI_MAX_CREDENTIALS) break;
            if (entry->string == NULL || !cJSON_IsString(entry) || entry->valuestring == NULL) continue;
            bruce_config_wifi_credential_t *credential = &cfg->wifiCredentials[cfg->wifiCredentialCount];
            credential->ssid = config__strdup(entry->string);
            credential->password = config__strdup(entry->valuestring);
            ++cfg->wifiCredentialCount;
        }
    }

    const cJSON *startup_apps = cJSON_GetObjectItemCaseSensitive(root, "startup");
    if (cJSON_IsArray(startup_apps)) {
        config__clear_string_array(cfg->startup.items, CONFIG__STARTUP_APP_MAX_COUNT);
        cfg->startup.count = 0;
        const cJSON *app;
        cJSON_ArrayForEach(app, startup_apps) {
            if (cfg->startup.count >= CONFIG__STARTUP_APP_MAX_COUNT) break;
            if (!cJSON_IsString(app) || app->valuestring == NULL) continue;
            if (!config__valid_value(app->valuestring, CONFIG__STARTUP_APP_MAX_LEN, false)) continue;
            cfg->startup.items[cfg->startup.count] = config__strdup(app->valuestring);
            ++cfg->startup.count;
        }
    }
    json_get_int(root, "devMode", &cfg->devMode);
}

static cJSON *config__build_json(const config__t *cfg) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    char hex[16];
    cJSON *theme = cJSON_AddObjectToObject(root, "theme");
    snprintf(hex, sizeof(hex), "%x", cfg->colorPrimary);
    cJSON_AddStringToObject(theme, "primary", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorSecondary);
    cJSON_AddStringToObject(theme, "secondary", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorBackground);
    cJSON_AddStringToObject(theme, "background", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorSurface);
    cJSON_AddStringToObject(theme, "surface", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorText);
    cJSON_AddStringToObject(theme, "text", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorTextMuted);
    cJSON_AddStringToObject(theme, "textMuted", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorBorder);
    cJSON_AddStringToObject(theme, "border", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorSuccess);
    cJSON_AddStringToObject(theme, "success", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorWarning);
    cJSON_AddStringToObject(theme, "warning", hex);
    snprintf(hex, sizeof(hex), "%x", cfg->colorError);
    cJSON_AddStringToObject(theme, "error", hex);

    cJSON *display = cJSON_AddObjectToObject(root, "display");
    cJSON_AddNumberToObject(display, "rotation", cfg->displayRotation);
    cJSON_AddNumberToObject(display, "dimTimeout", cfg->displayDimTimeout);
    cJSON_AddNumberToObject(display, "brightness", cfg->displayBrightness);
    cJSON_AddBoolToObject(display, "bufferedRendering", cfg->displayBufferedRendering);
    cJSON_AddBoolToObject(display, "dmaFramebuffer", cfg->displayDmaFramebuffer);

    cJSON *time = cJSON_AddObjectToObject(root, "time");
    cJSON_AddBoolToObject(time, "automaticUpdateViaNTP", cfg->timeAutomaticUpdateViaNTP);
    cJSON_AddNumberToObject(time, "timezone", cfg->timeTimezone);
    cJSON_AddBoolToObject(time, "dst", cfg->timeDst);
    cJSON_AddBoolToObject(time, "clock24hr", cfg->timeClock24hr);

    cJSON *sound = cJSON_AddObjectToObject(root, "sound");
    cJSON_AddBoolToObject(sound, "enabled", cfg->soundEnabled);
    cJSON_AddNumberToObject(sound, "volume", cfg->soundVolume);
    cJSON_AddStringToObject(root, "keyboardLang", config__or_empty(cfg->keyboardLang));

    cJSON *hotkeys = cJSON_AddObjectToObject(root, "hotkeys");
    for (size_t i = 0; i < cfg->hotkeys.count; ++i) {
        cJSON_AddStringToObject(
            hotkeys,
            config__or_empty(cfg->hotkeys.items[i].key),
            config__or_empty(cfg->hotkeys.items[i].action)
        );
    }

    cJSON *led = cJSON_AddObjectToObject(root, "led");
    cJSON_AddBoolToObject(led, "enabled", cfg->ledEnabled);
    cJSON_AddNumberToObject(led, "bright", cfg->ledBrightness);
    snprintf(hex, sizeof(hex), "%lx", (unsigned long)cfg->ledColor);
    cJSON_AddStringToObject(led, "color", hex);
    cJSON_AddNumberToObject(led, "blinkEnabled", cfg->ledBlinkEnabled);
    cJSON_AddNumberToObject(led, "effect", cfg->ledEffect);
    cJSON_AddNumberToObject(led, "effectSpeed", cfg->ledEffectSpeed);
    cJSON_AddNumberToObject(led, "effectDirection", cfg->ledEffectDirection);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "mac", config__or_empty(cfg->wifiMac));
    cJSON *wifi_ap = cJSON_AddObjectToObject(wifi, "ap");
    cJSON_AddStringToObject(wifi_ap, "ssid", config__or_empty(cfg->wifiApSsid));
    cJSON_AddStringToObject(wifi_ap, "pwd", config__or_empty(cfg->wifiApPassword));

    if (cfg->wifiCredentialCount > 0) {
        cJSON *credentials = cJSON_AddObjectToObject(wifi, "credentials");
        for (size_t i = 0; i < cfg->wifiCredentialCount; ++i) {
            cJSON_AddStringToObject(
                credentials,
                config__or_empty(cfg->wifiCredentials[i].ssid),
                config__or_empty(cfg->wifiCredentials[i].password)
            );
        }
    }

    cJSON *startup_apps = cJSON_AddArrayToObject(root, "startup");
    for (size_t i = 0; i < cfg->startup.count; ++i) {
        cJSON_AddItemToArray(startup_apps, cJSON_CreateString(config__or_empty(cfg->startup.items[i])));
    }
    cJSON_AddNumberToObject(root, "devMode", cfg->devMode);
    cJSON_AddStringToObject(root, "launcher", config__or_empty(cfg->launcher));

    return root;
}

/* ------------------------------------------------------------------------ */
/* Load / save                                                               */
/* ------------------------------------------------------------------------ */

static bool config__save_locked(void) {
    cJSON *root = config__build_json(&s_config);
    if (root == NULL) return false;

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL) return false;

    /* cJSON pretty printing uses a tab after each object key. */
    for (char *separator = strstr(text, ":\t"); separator != NULL; separator = strstr(separator + 2, ":\t")) {
        separator[1] = ' ';
    }

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
    if (!storage__mkdir_internal(CONFIG__DIRECTORY)) return false;
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

CONFIG__DEFINE_STRING_FIELD_GUARDED(wifi_mac, wifiMac, CONFIG__WIFI_MAC_MAX_LEN, config__guard_protected)

/* ---- `config`-permission-gated fields ------------------------------------ */

CONFIG__DEFINE_UINT16_FIELD(color_primary, colorPrimary)
CONFIG__DEFINE_UINT16_FIELD(color_secondary, colorSecondary)
CONFIG__DEFINE_UINT16_FIELD(color_background, colorBackground)
CONFIG__DEFINE_UINT16_FIELD(color_surface, colorSurface)
CONFIG__DEFINE_UINT16_FIELD(color_text, colorText)
CONFIG__DEFINE_UINT16_FIELD(color_text_muted, colorTextMuted)
CONFIG__DEFINE_UINT16_FIELD(color_border, colorBorder)
CONFIG__DEFINE_UINT16_FIELD(color_success, colorSuccess)
CONFIG__DEFINE_UINT16_FIELD(color_warning, colorWarning)
CONFIG__DEFINE_UINT16_FIELD(color_error, colorError)

bruce_result_t config__set_colors(const bruce_config_theme_colors_t *colors) {
    bruce_result_t guard = config__guard();
    if (guard != BRUCE_OK) return guard;
    if (colors == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!config__init()) return BRUCE_ERR_IO;
    config__lock();
    s_config.colorPrimary = colors->primary;
    s_config.colorSecondary = colors->secondary;
    s_config.colorBackground = colors->background;
    s_config.colorSurface = colors->surface;
    s_config.colorText = colors->text;
    s_config.colorTextMuted = colors->text_muted;
    s_config.colorBorder = colors->border;
    s_config.colorSuccess = colors->success;
    s_config.colorWarning = colors->warning;
    s_config.colorError = colors->error;
    config__validate(&s_config);
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

CONFIG__DEFINE_INT_FIELD(display_rotation, displayRotation)
CONFIG__DEFINE_BOOL_FIELD(display_buffered_rendering, displayBufferedRendering)
CONFIG__DEFINE_BOOL_FIELD(display_dma_framebuffer, displayDmaFramebuffer)
CONFIG__DEFINE_STRING_FIELD(launcher, launcher, CONFIG__LAUNCHER_APP_MAX_LEN)

CONFIG__DEFINE_INT_FIELD(display_dim_timeout, displayDimTimeout)
CONFIG__DEFINE_INT_FIELD(display_brightness, displayBrightness)
CONFIG__DEFINE_BOOL_FIELD(time_automatic_update_via_ntp, timeAutomaticUpdateViaNTP)
CONFIG__DEFINE_FLOAT_FIELD(time_timezone, timeTimezone)
CONFIG__DEFINE_BOOL_FIELD(time_dst, timeDst)
CONFIG__DEFINE_BOOL_FIELD(time_clock24hr, timeClock24hr)
CONFIG__DEFINE_BOOL_FIELD(sound_enabled, soundEnabled)
CONFIG__DEFINE_INT_FIELD(sound_volume, soundVolume)

void config__get_audio_settings(bool *enabled, int *volume) {
    if (enabled == NULL || volume == NULL) return;
    if (!config__init()) {
        *enabled = false;
        *volume = 0;
        return;
    }
    config__lock();
    *enabled = s_config.soundEnabled;
    *volume = s_config.soundVolume;
    config__unlock();
}

void config__get_colors_internal(
    uint16_t *pri, uint16_t *sec, uint16_t *bg, uint16_t *surface, uint16_t *text, uint16_t *text_muted,
    uint16_t *border, uint16_t *success, uint16_t *warning, uint16_t *error
) {
    if (pri == NULL || sec == NULL || bg == NULL || surface == NULL || text == NULL || text_muted == NULL ||
        border == NULL || success == NULL || warning == NULL || error == NULL)
        return;
    if (!config__init()) {
        *pri = 0;
        *sec = 0;
        *bg = 0;
        *surface = 0;
        *text = 0;
        *text_muted = 0;
        *border = 0;
        *success = 0;
        *warning = 0;
        *error = 0;
        return;
    }
    config__lock();
    *pri = s_config.colorPrimary;
    *sec = s_config.colorSecondary;
    *bg = s_config.colorBackground;
    *surface = s_config.colorSurface;
    *text = s_config.colorText;
    *text_muted = s_config.colorTextMuted;
    *border = s_config.colorBorder;
    *success = s_config.colorSuccess;
    *warning = s_config.colorWarning;
    *error = s_config.colorError;
    config__unlock();
}
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

CONFIG__DEFINE_BOOL_FIELD(led_enabled, ledEnabled)
CONFIG__DEFINE_INT_FIELD(led_brightness, ledBrightness)
CONFIG__DEFINE_UINT32_FIELD(led_color, ledColor)
CONFIG__DEFINE_BOOL_INT_FIELD(led_blink_enabled, ledBlinkEnabled)
CONFIG__DEFINE_INT_FIELD(led_effect, ledEffect)
CONFIG__DEFINE_INT_FIELD(led_effect_speed, ledEffectSpeed)
CONFIG__DEFINE_INT_FIELD(led_effect_direction, ledEffectDirection)

const bruce_config_startup_apps_t *config__get_startup_apps(void) {
    if (config__guard() != BRUCE_OK || !config__init()) return NULL;
    config__lock();
    const bruce_config_startup_apps_t *apps = &s_config.startup;
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
    config__clear_string_array(s_config.startup.items, CONFIG__STARTUP_APP_MAX_COUNT);
    memcpy(s_config.startup.items, copies, sizeof(copies));
    s_config.startup.count = count;
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
    for (size_t i = 0; i < s_config.startup.count; ++i) {
        if (strcmp(s_config.startup.items[i], key) == 0) {
            config__unlock();
            return BRUCE_OK;
        }
    }
    if (s_config.startup.count == CONFIG__STARTUP_APP_MAX_COUNT) {
        config__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    char *copy = config__strdup(key);
    if (copy == NULL) {
        config__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    s_config.startup.items[s_config.startup.count++] = copy;
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
    while (index < s_config.startup.count && strcmp(s_config.startup.items[index], key) != 0) {
        ++index;
    }
    if (index == s_config.startup.count) {
        config__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }

    config__release(&s_config.startup.items[index]);
    --s_config.startup.count;
    memmove(
        &s_config.startup.items[index],
        &s_config.startup.items[index + 1],
        (s_config.startup.count - index) * sizeof(s_config.startup.items[0])
    );
    s_config.startup.items[s_config.startup.count] = NULL;
    bool saved = config__save_locked();
    config__unlock();
    return saved ? BRUCE_OK : BRUCE_ERR_IO;
}

CONFIG__DEFINE_BOOL_INT_FIELD(dev_mode, devMode)
