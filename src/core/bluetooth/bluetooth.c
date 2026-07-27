#include "core/bluetooth/bluetooth.h"

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
static bluetooth__device_t *s_scan_devices;
static size_t s_scan_capacity;
static size_t s_scan_count;
static uint32_t s_scan_seconds;
static esp_err_t s_scan_start_error;
static bool s_scanning;

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
    if (!s_scanning || s_scan_capacity == 0) return;

    size_t index = s_scan_count;
    for (size_t i = 0; i < s_scan_count; ++i) {
        if (memcmp(s_scan_devices[i].address, result->bda, ESP_BD_ADDR_LEN) == 0) {
            index = i;
            break;
        }
    }
    if (index == s_scan_count) {
        if (s_scan_count >= s_scan_capacity) return;
        memset(&s_scan_devices[index], 0, sizeof(s_scan_devices[index]));
        memcpy(s_scan_devices[index].address, result->bda, ESP_BD_ADDR_LEN);
        s_scan_count++;
    }

    bluetooth__device_t *device = &s_scan_devices[index];
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
}

static void bluetooth__ble_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *parameter) {
    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        if (parameter->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_scan_start_error = esp_ble_gap_start_scanning(s_scan_seconds);
        } else {
            s_scan_start_error = ESP_FAIL;
        }
        if (s_scan_start_error != ESP_OK && s_events != NULL) {
            xEventGroupSetBits(s_events, BLUETOOTH__SCAN_DONE_BIT);
        }
    } else if (event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
        if (parameter->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            s_scan_start_error = ESP_FAIL;
            xEventGroupSetBits(s_events, BLUETOOTH__SCAN_DONE_BIT);
        }
    } else if (event == ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT) {
        xEventGroupSetBits(s_events, BLUETOOTH__SCAN_STOPPED_BIT);
    } else if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
        if (parameter->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            bluetooth__collect_scan_result(&parameter->scan_rst);
        } else if (parameter->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT && s_events != NULL) {
            xEventGroupSetBits(s_events, BLUETOOTH__SCAN_DONE_BIT);
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

bruce_result_t bluetooth__init(void) { return bluetooth__stack_init(); }

static int bluetooth__compare_rssi(const void *left, const void *right) {
    const bluetooth__device_t *a = left;
    const bluetooth__device_t *b = right;
    return (int)b->rssi - (int)a->rssi;
}

int bluetooth__scan_ble(bluetooth__device_t *devices, size_t capacity, uint32_t timeout_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_BT);
    if (permission != BRUCE_OK) return permission;
    if ((capacity > 0 && devices == NULL) || capacity > UINT16_MAX) return BRUCE_ERR_INVALID_ARGUMENT;
    if (timeout_ms == 0) timeout_ms = BLUETOOTH__DEFAULT_SCAN_MS;
    if (timeout_ms > BLUETOOTH__MAX_SCAN_MS) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_result_t initialized = bluetooth__stack_init();
    if (initialized != BRUCE_OK) return initialized;

    bluetooth__lock();
    if (!s_gap_registered || s_scanning) {
        bluetooth__unlock();
        return BRUCE_ERR_BUSY;
    }
    if (s_scan_devices == NULL) {
        s_scan_devices = calloc(BLUETOOTH__MAX_RESULTS, sizeof(bluetooth__device_t));
        if (s_scan_devices == NULL) {
            bluetooth__unlock();
            return BRUCE_ERR_NO_MEMORY;
        }
    }
    s_scan_capacity = capacity < BLUETOOTH__MAX_RESULTS ? capacity : BLUETOOTH__MAX_RESULTS;
    s_scan_count = 0;
    s_scan_seconds = (timeout_ms + 999) / 1000;
    s_scan_start_error = ESP_OK;
    memset(s_scan_devices, 0, BLUETOOTH__MAX_RESULTS * sizeof(bluetooth__device_t));
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
    esp_err_t error = esp_ble_gap_set_scan_params(&parameters);
    bluetooth__unlock();

    if (error != ESP_OK) {
        bluetooth__lock();
        s_scanning = false;
        bluetooth__unlock();
        return error == ESP_ERR_INVALID_STATE ? BRUCE_ERR_BUSY : BRUCE_ERR_IO;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_events, BLUETOOTH__SCAN_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms + 1500)
    );
    if ((bits & BLUETOOTH__SCAN_DONE_BIT) == 0) {
        if (esp_ble_gap_stop_scanning() == ESP_OK) {
            (void)xEventGroupWaitBits(s_events, BLUETOOTH__SCAN_STOPPED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        }
    }

    bluetooth__lock();
    int count = (int)s_scan_count;
    if (devices != NULL && count > 0) memcpy(devices, s_scan_devices, (size_t)count * sizeof(*devices));
    s_scanning = false;
    s_scan_capacity = 0;
    s_scan_count = 0;
    bluetooth__unlock();
    if (count > 1) qsort(devices, (size_t)count, sizeof(*devices), bluetooth__compare_rssi);
    if (s_scan_start_error != ESP_OK) return BRUCE_ERR_IO;
    return (bits & BLUETOOTH__SCAN_DONE_BIT) != 0 ? count : BRUCE_ERR_TIMEOUT;
}
