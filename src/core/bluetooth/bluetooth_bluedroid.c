/* Bluedroid backend for BLE advertisement scanning. Compiled in only when
 * CONFIG_BT_NIMBLE_ENABLED is off (see src/CMakeLists.txt) -- currently the
 * original ESP32 target, which also needs Bluedroid for Classic Bluetooth
 * HID host support (bluetooth_hid.c). See bluetooth_nimble.c for the NimBLE
 * backend used on every BLE-only target. Both implement the exact same
 * bluetooth__stack_init()/bluetooth__scan_ble() contract declared in
 * bluetooth_internal.h / core_sdk/bluetooth.h, so bluetooth.c's facade and
 * every caller of bluetooth__scan_ble() are backend-agnostic. */

#include <stdlib.h>
#include <string.h>

#include "core/bluetooth/bluetooth_internal.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/permission.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

#define BLUETOOTH__SCAN_DONE_BIT BIT0
#define BLUETOOTH__SCAN_STOPPED_BIT BIT1
#define BLUETOOTH__DEFAULT_SCAN_MS 5000
#define BLUETOOTH__MAX_SCAN_MS 30000
#define BLUETOOTH__MAX_RESULTS 64

static const char *const TAG = "bruce_bt";
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_gap_registered;
static uint32_t s_scan_seconds;
static esp_err_t s_scan_start_error;
static bool s_scanning;

/* Only one scan can be in flight on the radio; concurrent
 * bluetooth__scan_start() callers join the same round rather than
 * restarting it. s_scan_refcount is how many haven't collected it yet;
 * s_scan_cache is that round's results, filled live as advertisements
 * arrive (see bluetooth__collect_scan_result()) and shared out (guarded by
 * s_mutex) once BLUETOOTH__SCAN_DONE_BIT is set. */
static int s_scan_refcount;
static bluetooth__device_t s_scan_cache[BLUETOOTH__MAX_RESULTS];
static size_t s_scan_cache_count;

static void bluetooth__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void bluetooth__unlock(void) { xSemaphoreGive(s_mutex); }

static void bluetooth__copy_name(
    char *destination, size_t destination_size, const uint8_t *source, size_t source_length
) {
    if (destination == NULL || destination_size == 0) return;
    size_t length = source_length < destination_size - 1 ? source_length : destination_size - 1;
    if (source != NULL && length > 0) memcpy(destination, source, length);
    destination[length] = '\0';
}

static void bluetooth__collect_scan_result(const struct ble_scan_result_evt_param *result) {
    bluetooth__lock();
    if (!s_scanning) {
        bluetooth__unlock();
        return;
    }

    size_t index = s_scan_cache_count;
    for (size_t i = 0; i < s_scan_cache_count; ++i) {
        if (memcmp(s_scan_cache[i].address, result->bda, ESP_BD_ADDR_LEN) == 0) {
            index = i;
            break;
        }
    }
    if (index == s_scan_cache_count) {
        if (s_scan_cache_count >= BLUETOOTH__MAX_RESULTS) {
            bluetooth__unlock();
            return;
        }
        memset(&s_scan_cache[index], 0, sizeof(s_scan_cache[index]));
        memcpy(s_scan_cache[index].address, result->bda, ESP_BD_ADDR_LEN);
        s_scan_cache_count++;
    }

    bluetooth__device_t *device = &s_scan_cache[index];
    device->address_type = result->ble_addr_type;
    device->rssi = result->rssi;

    uint8_t name_length = 0;
    uint16_t advertisement_length = result->adv_data_len + result->scan_rsp_len;
    uint8_t *name = esp_ble_resolve_adv_data_by_type(
        (uint8_t *)result->ble_adv, advertisement_length, ESP_BLE_AD_TYPE_NAME_CMPL, &name_length
    );
    if (name == NULL) {
        name = esp_ble_resolve_adv_data_by_type(
            (uint8_t *)result->ble_adv, advertisement_length, ESP_BLE_AD_TYPE_NAME_SHORT, &name_length
        );
    }
    if (name != NULL) bluetooth__copy_name(device->name, sizeof(device->name), name, name_length);
    bluetooth__unlock();
}

static void bluetooth__ble_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *parameter) {
    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        bluetooth__lock();
        if (!s_scanning) {
            bluetooth__unlock();
            return;
        }
        if (parameter->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_scan_start_error = esp_ble_gap_start_scanning(s_scan_seconds);
        } else {
            s_scan_start_error = ESP_FAIL;
        }
        if (s_scan_start_error != ESP_OK) {
            s_scanning = false;
            if (s_events != NULL) xEventGroupSetBits(s_events, BLUETOOTH__SCAN_DONE_BIT);
        }
        bluetooth__unlock();
    } else if (event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
        bluetooth__lock();
        if (parameter->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            s_scan_start_error = ESP_FAIL;
            s_scanning = false;
            xEventGroupSetBits(s_events, BLUETOOTH__SCAN_DONE_BIT);
        }
        bluetooth__unlock();
    } else if (event == ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT) {
        xEventGroupSetBits(s_events, BLUETOOTH__SCAN_STOPPED_BIT);
    } else if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
        if (parameter->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            bluetooth__collect_scan_result(&parameter->scan_rst);
        } else if (parameter->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            bluetooth__lock();
            s_scanning = false;
            bluetooth__unlock();
            if (s_events != NULL) xEventGroupSetBits(s_events, BLUETOOTH__SCAN_DONE_BIT);
        }
    }
}

bruce_result_t bluetooth__stack_init(void) {
    bluetooth__lock();
    if (s_initialized) {
        bluetooth__unlock();
        return BRUCE_OK;
    }

    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) error = nvs_flash_init();
    }
    if (error != ESP_OK) {
        bluetooth__unlock();
        return BRUCE_ERR_IO;
    }

    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    error = esp_bt_controller_init(&controller_config);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(error));
        bluetooth__unlock();
        return BRUCE_ERR_IO;
    }
#if CONFIG_BT_CLASSIC_ENABLED
    const esp_bt_mode_t mode = ESP_BT_MODE_BTDM;
#else
    const esp_bt_mode_t mode = ESP_BT_MODE_BLE;
#endif
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        error = esp_bt_controller_enable(mode);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(error));
            bluetooth__unlock();
            return BRUCE_ERR_IO;
        }
    }

    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    error = esp_bluedroid_init_with_cfg(&bluedroid_config);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(error));
        bluetooth__unlock();
        return BRUCE_ERR_IO;
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        error = esp_bluedroid_enable();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(error));
            bluetooth__unlock();
            return BRUCE_ERR_IO;
        }
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        bluetooth__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    error = esp_ble_gap_register_callback(bluetooth__ble_gap_callback);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "BLE GAP callback registration failed: %s", esp_err_to_name(error));
        bluetooth__unlock();
        return BRUCE_ERR_IO;
    }
    s_gap_registered = true;
    s_initialized = true;
    bluetooth__unlock();
    return BRUCE_OK;
}

static int bluetooth__compare_rssi(const void *left, const void *right) {
    const bluetooth__device_t *a = left;
    const bluetooth__device_t *b = right;
    return (int)b->rssi - (int)a->rssi;
}

bruce_result_t bluetooth__scan_start(uint32_t timeout_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_BT);
    if (permission != BRUCE_OK) return permission;
    if (timeout_ms == 0) timeout_ms = BLUETOOTH__DEFAULT_SCAN_MS;
    if (timeout_ms > BLUETOOTH__MAX_SCAN_MS) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_result_t initialized = bluetooth__stack_init();
    if (initialized != BRUCE_OK) return initialized;

    bluetooth__lock();
    if (!s_gap_registered) {
        bluetooth__unlock();
        return BRUCE_ERR_BUSY;
    }
    if (s_scan_refcount == 0) {
        /* Fresh round: only clear the bits here, never in
         * bluetooth__scan_poll(), so multiple waiters can safely observe
         * the same completion. */
        s_scan_cache_count = 0;
        s_scan_seconds = (timeout_ms + 999) / 1000;
        s_scan_start_error = ESP_OK;
        s_scanning = true;
        xEventGroupClearBits(s_events, BLUETOOTH__SCAN_DONE_BIT | BLUETOOTH__SCAN_STOPPED_BIT);

        esp_ble_scan_params_t parameters = {
            .scan_type = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval = 0x50,
            .scan_window = 0x30,
            .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
        };
        /* Non-blocking: scan params/start complete asynchronously through
         * bluetooth__ble_gap_callback(), which kicks off the actual scan and
         * reports each advertisement (and eventual completion) from there --
         * we don't sit here holding s_mutex for the whole scan. */
        esp_err_t error = esp_ble_gap_set_scan_params(&parameters);
        if (error != ESP_OK) {
            s_scanning = false;
            bluetooth__unlock();
            return error == ESP_ERR_INVALID_STATE ? BRUCE_ERR_BUSY : BRUCE_ERR_IO;
        }
    } /* else: a round is already in flight or uncollected -- join it instead
       * of restarting the radio scan out from under that caller. */
    s_scan_refcount++;
    bluetooth__unlock();
    return BRUCE_OK;
}

int bluetooth__scan_poll(bluetooth__device_t *devices, size_t capacity, uint32_t timeout_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_BT);
    if (permission != BRUCE_OK) return (int)permission;
    if (capacity != 0 && devices == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (s_events == NULL) return BRUCE_ERR_IO;

    /* pdFALSE clear-on-exit: FreeRTOS doesn't reliably wake multiple tasks
     * blocked on the same auto-clearing bit, so this stays level-triggered
     * until bluetooth__scan_start() clears it for the next round. A timeout
     * here leaves the round untouched -- call again to keep waiting. */
    EventBits_t bits = xEventGroupWaitBits(
        s_events, BLUETOOTH__SCAN_DONE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)
    );
    if ((bits & BLUETOOTH__SCAN_DONE_BIT) == 0) { return BRUCE_ERR_TIMEOUT; }

    bluetooth__lock();
    esp_err_t start_error = s_scan_start_error;
    if (s_scan_cache_count > 1) qsort(s_scan_cache, s_scan_cache_count, sizeof(*s_scan_cache), bluetooth__compare_rssi);
    int count = (int)s_scan_cache_count;
    if (start_error == ESP_OK && devices != NULL) {
        size_t to_copy = (size_t)count < capacity ? (size_t)count : capacity;
        for (size_t i = 0; i < to_copy; ++i) { devices[i] = s_scan_cache[i]; }
        count = (int)to_copy;
    } else {
        count = 0;
    }
    if (s_scan_refcount > 0) s_scan_refcount--;
    bluetooth__unlock();
    return start_error == ESP_OK ? count : BRUCE_ERR_IO;
}

bruce_result_t bluetooth__scan_cancel(void) {
    if (s_events == NULL) return BRUCE_OK;
    bluetooth__lock();
    if (s_scan_refcount > 0) s_scan_refcount--;
    bool last_waiter = s_scan_refcount == 0;
    bool was_scanning = s_scanning;
    if (last_waiter) s_scanning = false;
    bluetooth__unlock();
    if (!last_waiter) return BRUCE_OK; /* someone else is still waiting on this round */
    if (!was_scanning) return BRUCE_OK; /* already finished on its own */
    if (esp_ble_gap_stop_scanning() != ESP_OK) return BRUCE_ERR_IO;
    (void)xEventGroupWaitBits(s_events, BLUETOOTH__SCAN_STOPPED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
    return BRUCE_OK; /* best-effort */
}

int bluetooth__scan_ble(bluetooth__device_t *devices, size_t capacity, uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = BLUETOOTH__DEFAULT_SCAN_MS;
    bruce_result_t start = bluetooth__scan_start(timeout_ms);
    if (start != BRUCE_OK) return (int)start;
    int result = bluetooth__scan_poll(devices, capacity, timeout_ms + 1500);
    if (result == BRUCE_ERR_TIMEOUT) { (void)bluetooth__scan_cancel(); }
    return result;
}
