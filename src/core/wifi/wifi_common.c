#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config/config.h"
#include "core/clock/clock.h"
#include "core/network/network.h"
#include "core/wifi/wifi_common.h"
#include "core_sdk/config.h"
#include "core_sdk/permission.h"
#include "core_sdk/status_icon.h"
#include "core_sdk/wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#define WIFI__CONNECTED_BIT BIT0
#define WIFI__AP_BIT BIT1
#define WIFI__SCAN_DONE_BIT BIT2
#define WIFI__DEFAULT_CONNECT_TIMEOUT_MS 10000
#define WIFI__SCAN_TIMEOUT_MS 10000
/* Caps how many networks a shared scan round keeps in s_scan_cache -- every
 * current caller asks for <=32, so this never truncates anyone in practice
 * (see wifi__scan_poll()). */
#define WIFI__SCAN_CACHE_MAX 32
#define WIFI__STATUS_ICON_KEY "core.wifi"

static const char *const TAG = "bruce_wifi";
static StaticSemaphore_t s_wifi_mutex_storage;
static SemaphoreHandle_t s_wifi_mutex;
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_initialized;
static bool s_started;
static bool s_ap_running;
static bool s_event_loop_owned;
static char s_active_ssid[CONFIG__WIFI_SSID_MAX_LEN + 1];
static char s_ip_buffer[16];
static char s_mac_buffer[18];

/* Only one scan can be in flight on the radio; concurrent wifi__scan_start()
 * callers join the same round rather than restarting it. s_scan_refcount is
 * how many haven't resolved it yet; s_scan_cache is that round's results,
 * fetched from the driver once and shared out (guarded by s_wifi_mutex). */
static int s_scan_refcount;
static bool s_scan_cache_ready;
static int s_scan_cache_result;
static wifi__network_t s_scan_cache[WIFI__SCAN_CACHE_MAX];

static const uint8_t s_wifi_status_icon[] = {
    0x3F, 0xE0,
    0x60, 0x30,
    0x00, 0x00,
    0x0F, 0x80,
    0x18, 0xC0,
    0x00, 0x00,
    0x07, 0x00,
    0x07, 0x00,
};

static void wifi__set_connected(bool connected) {
    if (connected) {
        xEventGroupSetBits(s_wifi_events, WIFI__CONNECTED_BIT);
        (void)status_icon__push(WIFI__STATUS_ICON_KEY, s_wifi_status_icon, 13, 8);
    } else {
        xEventGroupClearBits(s_wifi_events, WIFI__CONNECTED_BIT);
        (void)status_icon__remove(WIFI__STATUS_ICON_KEY);
    }
}

static void wifi__copy(char *destination, size_t destination_size, const char *source) {
    if (destination == NULL || destination_size == 0) { return; }
    strncpy(destination, source != NULL ? source : "", destination_size - 1);
    destination[destination_size - 1] = '\0';
}

static void wifi__event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (!s_initialized || s_wifi_events == NULL) {
        xSemaphoreGive(s_wifi_mutex);
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi__set_connected(false);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi__set_connected(true);
        clock__notify_network_connected();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(s_wifi_events, WIFI__AP_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        xEventGroupClearBits(s_wifi_events, WIFI__AP_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_wifi_events, WIFI__SCAN_DONE_BIT);
    }
    xSemaphoreGive(s_wifi_mutex);
}

static bool wifi__start_locked(wifi_mode_t mode) {
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not select Wi-Fi mode: %s", esp_err_to_name(err));
        return false;
    }
    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not start Wi-Fi: %s", esp_err_to_name(err));
            return false;
        }
        s_started = true;
    }
    return true;
}

bruce_result_t wifi__init(void) {
    if (s_wifi_mutex == NULL) { s_wifi_mutex = xSemaphoreCreateMutexStatic(&s_wifi_mutex_storage); }
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_OK;
    }

    if (!config__init()) {
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_IO;
    }
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_NO_MEMORY;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not initialize NVS: %s", esp_err_to_name(err));
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_IO;
    }

    bruce_result_t network_result = network__init();
    if (network_result != BRUCE_OK) {
        ESP_LOGE(TAG, "could not initialize network stack");
        xSemaphoreGive(s_wifi_mutex);
        return network_result;
    }
    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        s_event_loop_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "could not create event loop: %s", esp_err_to_name(err));
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_IO;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "could not create station netif");
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_NO_MEMORY;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "could not create access-point netif");
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_NO_MEMORY;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not initialize Wi-Fi: %s", esp_err_to_name(err));
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_IO;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi__event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi__event_handler, NULL);
    s_initialized = true;
    xSemaphoreGive(s_wifi_mutex);
    return BRUCE_OK;
}

bruce_result_t wifi__disconnect(void) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_WIFI);
    if (result != BRUCE_OK) return result;
    if (s_wifi_mutex == NULL) {
        (void)status_icon__remove(WIFI__STATUS_ICON_KEY);
        return BRUCE_OK;
    }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (!s_initialized) {
        xSemaphoreGive(s_wifi_mutex);
        (void)status_icon__remove(WIFI__STATUS_ICON_KEY);
        return BRUCE_OK;
    }

    wifi__set_connected(false);
    xEventGroupClearBits(s_wifi_events, WIFI__AP_BIT);
    (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi__event_handler);
    (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi__event_handler);

    esp_err_t stop_err = ESP_OK;
    if (s_started) {
        (void)esp_wifi_disconnect();
        stop_err = esp_wifi_stop();
        s_started = false;
    }
    esp_err_t deinit_err = esp_wifi_deinit();
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_event_loop_owned && esp_event_loop_delete_default() == ESP_OK) s_event_loop_owned = false;
    vEventGroupDelete(s_wifi_events);
    s_wifi_events = NULL;
    s_initialized = false;
    s_ap_running = false;
    s_active_ssid[0] = '\0';
    xSemaphoreGive(s_wifi_mutex);
    return stop_err == ESP_OK && deinit_err == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t wifi__connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_WIFI);
    if (result != BRUCE_OK) return result;
    if (ssid == NULL || ssid[0] == '\0' ||
        strnlen(ssid, CONFIG__WIFI_SSID_MAX_LEN + 1) > CONFIG__WIFI_SSID_MAX_LEN || password == NULL ||
        strnlen(password, CONFIG__WIFI_PASSWORD_MAX_LEN + 1) > CONFIG__WIFI_PASSWORD_MAX_LEN) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    result = wifi__init();
    if (result != BRUCE_OK) return result;

    wifi_config_t station = {0};
    wifi__copy((char *)station.sta.ssid, sizeof(station.sta.ssid), ssid);
    wifi__copy((char *)station.sta.password, sizeof(station.sta.password), password);

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (!wifi__start_locked(WIFI_MODE_STA) || esp_wifi_set_config(WIFI_IF_STA, &station) != ESP_OK) {
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_IO;
    }
    s_ap_running = false;
    wifi__set_connected(false);
    esp_err_t err = esp_wifi_connect();
    xSemaphoreGive(s_wifi_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not connect to %s: %s", ssid, esp_err_to_name(err));
        return BRUCE_ERR_IO;
    }

    if (timeout_ms == 0) { timeout_ms = WIFI__DEFAULT_CONNECT_TIMEOUT_MS; }
    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_events, WIFI__CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI__CONNECTED_BIT) == 0) { return BRUCE_ERR_TIMEOUT; }
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    wifi__copy(s_active_ssid, sizeof(s_active_ssid), ssid);
    xSemaphoreGive(s_wifi_mutex);
    return BRUCE_OK;
}

bruce_result_t wifi__scan_start(void) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_WIFI);
    if (result != BRUCE_OK) return result;
    result = wifi__init();
    if (result != BRUCE_OK) return result;
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (!wifi__start_locked(WIFI_MODE_STA)) {
        xSemaphoreGive(s_wifi_mutex);
        return BRUCE_ERR_IO;
    }
    if (s_scan_refcount == 0) {
        /* Fresh round: only clear the bit here, never in wifi__scan_poll(),
         * so multiple waiters can safely observe the same completion. */
        xEventGroupClearBits(s_wifi_events, WIFI__SCAN_DONE_BIT);
        s_scan_cache_ready = false;
        /* Non-blocking: esp_wifi scans on its own task and posts
         * WIFI_EVENT_SCAN_DONE, so we don't sit here holding s_wifi_mutex
         * for the whole scan (that used to stall every other wifi__*
         * caller in the system for as long as the scan took). */
        esp_err_t err = esp_wifi_scan_start(NULL, false);
        if (err != ESP_OK) {
            xSemaphoreGive(s_wifi_mutex);
            return BRUCE_ERR_IO;
        }
    } /* else: a round is already in flight or uncollected -- join it instead
       * of restarting the radio scan out from under that caller. */
    s_scan_refcount++;
    xSemaphoreGive(s_wifi_mutex);
    return BRUCE_OK;
}

/* s_wifi_mutex held. Runs once per round (guarded by s_scan_cache_ready):
 * fetches from the driver into s_scan_cache, freeing its AP list as a
 * side effect, so other waiters copy from the cache instead of each
 * fetching (and re-freeing) the same driver-owned list. */
static void wifi__scan_fill_cache_locked(void) {
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    uint16_t to_cache = found > WIFI__SCAN_CACHE_MAX ? WIFI__SCAN_CACHE_MAX : found;
    s_scan_cache_result = 0;
    if (to_cache > 0) {
        wifi_ap_record_t *records = calloc(to_cache, sizeof(*records));
        if (records == NULL) {
            (void)esp_wifi_clear_ap_list();
            s_scan_cache_result = BRUCE_ERR_NO_MEMORY;
        } else {
            uint16_t record_count = to_cache;
            esp_err_t err = esp_wifi_scan_get_ap_records(&record_count, records);
            if (err == ESP_OK) {
                for (uint16_t i = 0; i < record_count; ++i) {
                    wifi__copy(
                        s_scan_cache[i].ssid, sizeof(s_scan_cache[i].ssid), (const char *)records[i].ssid
                    );
                    s_scan_cache[i].rssi = records[i].rssi;
                    s_scan_cache[i].channel = records[i].primary;
                    s_scan_cache[i].authmode = (uint8_t)records[i].authmode;
                }
                s_scan_cache_result = (int)record_count;
            } else {
                s_scan_cache_result = BRUCE_ERR_IO;
            }
            free(records);
        }
    } else if (found > 0) {
        (void)esp_wifi_clear_ap_list(); /* over WIFI__SCAN_CACHE_MAX -- still free the driver's list */
    }
    s_scan_cache_ready = true;
}

int wifi__scan_poll(wifi__network_t *networks, size_t capacity, uint32_t timeout_ms) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_WIFI);
    if (result != BRUCE_OK) return (int)result;
    if (capacity != 0 && networks == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (s_wifi_events == NULL) return BRUCE_ERR_IO;

    /* pdFALSE clear-on-exit: FreeRTOS doesn't reliably wake multiple tasks
     * blocked on the same auto-clearing bit, so this stays level-triggered
     * until wifi__scan_start() clears it for the next round. A timeout here
     * leaves the round untouched -- call again to keep waiting. */
    EventBits_t scan_bits = xEventGroupWaitBits(
        s_wifi_events, WIFI__SCAN_DONE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)
    );
    if ((scan_bits & WIFI__SCAN_DONE_BIT) == 0) { return BRUCE_ERR_TIMEOUT; }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (!s_scan_cache_ready) { wifi__scan_fill_cache_locked(); }

    int scan_result = s_scan_cache_result;
    if (scan_result > 0) {
        if (networks != NULL) {
            size_t to_copy = (size_t)scan_result < capacity ? (size_t)scan_result : capacity;
            for (size_t i = 0; i < to_copy; ++i) { networks[i] = s_scan_cache[i]; }
            scan_result = (int)to_copy;
        } else {
            scan_result = 0; /* caller didn't want records (capacity 0 / networks NULL) */
        }
    }

    /* Resolved -- once every joiner has too, drop the cache so the next
     * wifi__scan_start() starts a fresh scan instead of reusing this one. */
    if (s_scan_refcount > 0) s_scan_refcount--;
    if (s_scan_refcount == 0) s_scan_cache_ready = false;
    xSemaphoreGive(s_wifi_mutex);
    return scan_result;
}

bruce_result_t wifi__scan_cancel(void) {
    if (s_wifi_events == NULL) return BRUCE_OK;
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (s_scan_refcount > 0) s_scan_refcount--;
    bool last_waiter = s_scan_refcount == 0;
    if (last_waiter) s_scan_cache_ready = false;
    xSemaphoreGive(s_wifi_mutex);
    if (!last_waiter) return BRUCE_OK; /* someone else is still waiting on this round */
    return esp_wifi_scan_stop() == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO; /* best-effort */
}

int wifi__scan(wifi__network_t *networks, size_t capacity) {
    bruce_result_t result = wifi__scan_start();
    if (result != BRUCE_OK) return (int)result;
    int scan_result = wifi__scan_poll(networks, capacity, WIFI__SCAN_TIMEOUT_MS);
    if (scan_result == BRUCE_ERR_TIMEOUT) { (void)wifi__scan_cancel(); }
    return scan_result;
}

bruce_result_t wifi__connect_known(void) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_WIFI);
    if (result != BRUCE_OK) return result;
    wifi__network_t networks[16];
    int count = wifi__scan(networks, 16);
    if (count < 0) return (bruce_result_t)count;
    for (int i = 0; i < count; ++i) {
        const bruce_config_wifi_credential_t *credential =
            config__find_wifi_credential(networks[i].ssid);
        if (credential != NULL) {
            bruce_result_t result =
                wifi__connect(credential->ssid, credential->password, WIFI__DEFAULT_CONNECT_TIMEOUT_MS);
            if (result == BRUCE_OK) return BRUCE_OK;
        }
    }
    return BRUCE_ERR_NOT_FOUND;
}

bruce_result_t wifi__setup_ap(void) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_WIFI);
    if (result != BRUCE_OK) return result;
    result = wifi__init();
    if (result != BRUCE_OK) return result;
    const char *ssid = config__get_wifi_ap_ssid();
    const char *password = config__get_wifi_ap_password();
    if (ssid == NULL || password == NULL) return BRUCE_ERR_IO;
    wifi_config_t ap = {0};
    wifi__copy((char *)ap.ap.ssid, sizeof(ap.ap.ssid), ssid);
    wifi__copy((char *)ap.ap.password, sizeof(ap.ap.password), password);
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    bool started = wifi__start_locked(WIFI_MODE_AP) && esp_wifi_set_config(WIFI_IF_AP, &ap) == ESP_OK;
    if (started) {
        s_ap_running = true;
        wifi__copy(s_active_ssid, sizeof(s_active_ssid), ssid);
    }
    xSemaphoreGive(s_wifi_mutex);
    return started ? BRUCE_OK : BRUCE_ERR_IO;
}

bool wifi__is_connected(void) {
    if (permission__check(BRUCE_PERMISSION_WIFI) != BRUCE_OK) return false;
    return wifi__is_connected_internal();
}

bool wifi__is_connected_internal(void) {
    return s_wifi_events != NULL && (xEventGroupGetBits(s_wifi_events) & WIFI__CONNECTED_BIT) != 0;
}

bool wifi__is_ap_running(void) {
    if (permission__check(BRUCE_PERMISSION_WIFI) != BRUCE_OK) return false;
    return s_wifi_events != NULL && (xEventGroupGetBits(s_wifi_events) & WIFI__AP_BIT) != 0;
}

const char *wifi__get_ssid(void) {
    if (permission__check(BRUCE_PERMISSION_WIFI) != BRUCE_OK) return NULL;
    if (wifi__init() != BRUCE_OK) return NULL;
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    bool present = s_active_ssid[0] != '\0';
    xSemaphoreGive(s_wifi_mutex);
    return present ? s_active_ssid : NULL;
}

const char *wifi__get_ip(void) {
    if (permission__check(BRUCE_PERMISSION_WIFI) != BRUCE_OK) return NULL;
    if (wifi__init() != BRUCE_OK) return NULL;
    esp_netif_t *netif = NULL;
    if (wifi__is_connected()) {
        netif = s_sta_netif;
    } else if (wifi__is_ap_running()) {
        netif = s_ap_netif;
    }
    esp_netif_ip_info_t info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &info) != ESP_OK) { return NULL; }
    return inet_ntoa_r(info.ip, s_ip_buffer, sizeof(s_ip_buffer)) != NULL ? s_ip_buffer : NULL;
}

const char *wifi__get_mac(void) {
    if (permission__check(BRUCE_PERMISSION_WIFI) != BRUCE_OK) return NULL;
    uint8_t address[6];
    if (wifi__init() != BRUCE_OK) return NULL;
    if (esp_wifi_get_mac(WIFI_IF_STA, address) != ESP_OK) return NULL;
    snprintf(
        s_mac_buffer,
        sizeof(s_mac_buffer),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        address[0],
        address[1],
        address[2],
        address[3],
        address[4],
        address[5]
    );
    return s_mac_buffer;
}
