/* NimBLE backend for BLE advertisement scanning. Compiled in only when
 * CONFIG_BT_NIMBLE_ENABLED is on (see src/CMakeLists.txt) -- every BLE-only
 * target (no Classic Bluetooth radio, so no use for Bluedroid's dual-mode
 * footprint). See bluetooth_bluedroid.c for the Bluedroid backend used on
 * targets that also need Classic Bluetooth HID host support (bluetooth_hid.c).
 * Both implement the exact same bluetooth__stack_init()/bluetooth__scan_ble()
 * contract declared in bluetooth_internal.h / core_sdk/bluetooth.h, so
 * bluetooth.c's facade and every caller of bluetooth__scan_ble() are
 * backend-agnostic.
 *
 * Modeled directly on ESP-IDF's own "blecent" example (the stock BLE
 * central/scan reference) -- see examples/bluetooth/nimble/blecent in the
 * ESP-IDF tree this was built against. */

#include <stdlib.h>
#include <string.h>

#include "core/bluetooth/bluetooth_internal.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/permission.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define BLUETOOTH__SYNC_DONE_BIT BIT0
#define BLUETOOTH__SCAN_DONE_BIT BIT1
#define BLUETOOTH__SYNC_TIMEOUT_MS 5000
#define BLUETOOTH__DEFAULT_SCAN_MS 5000
#define BLUETOOTH__MAX_SCAN_MS 30000
#define BLUETOOTH__MAX_RESULTS 64

static const char *const TAG = "bruce_bt";
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static uint8_t s_own_addr_type;
static bluetooth__device_t *s_scan_devices;
static size_t s_scan_capacity;
static size_t s_scan_count;
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

/* NimBLE stores ble_addr_t.val[] in on-air (little-endian) order -- reversed
 * from the left-to-right MAC order every caller of bluetooth__scan_ble()
 * expects (and that the Bluedroid backend already produces directly from
 * esp_bd_addr_t). Reverse on the way in so both backends hand out identical
 * address bytes. */
static void bluetooth__copy_address(uint8_t destination[BRUCE_BLUETOOTH_ADDRESS_LEN], const ble_addr_t *source) {
    for (size_t i = 0; i < BRUCE_BLUETOOTH_ADDRESS_LEN; ++i) {
        destination[i] = source->val[BRUCE_BLUETOOTH_ADDRESS_LEN - 1 - i];
    }
}

static void bluetooth__collect_scan_result(const struct ble_gap_disc_desc *result) {
    bluetooth__lock();
    if (!s_scanning || s_scan_devices == NULL || s_scan_capacity == 0) {
        bluetooth__unlock();
        return;
    }

    uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN];
    bluetooth__copy_address(address, &result->addr);

    size_t index = s_scan_count;
    for (size_t i = 0; i < s_scan_count; ++i) {
        if (memcmp(s_scan_devices[i].address, address, sizeof(address)) == 0) {
            index = i;
            break;
        }
    }
    if (index == s_scan_count) {
        if (s_scan_count >= s_scan_capacity) {
            bluetooth__unlock();
            return;
        }
        memset(&s_scan_devices[index], 0, sizeof(s_scan_devices[index]));
        memcpy(s_scan_devices[index].address, address, sizeof(address));
        s_scan_count++;
    }

    bluetooth__device_t *device = &s_scan_devices[index];
    device->address_type = result->addr.type;
    device->rssi = result->rssi;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, result->data, result->length_data) == 0 && fields.name != NULL) {
        bluetooth__copy_name(device->name, sizeof(device->name), fields.name, fields.name_len);
    }
    bluetooth__unlock();
}

static int bluetooth__gap_event(struct ble_gap_event *event, void *argument) {
    (void)argument;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: bluetooth__collect_scan_result(&event->disc); break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (s_events != NULL) xEventGroupSetBits(s_events, BLUETOOTH__SCAN_DONE_BIT);
            break;
        default: break;
    }
    return 0;
}

static void bluetooth__on_reset(int reason) { ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason); }

static void bluetooth__on_sync(void) {
    int error = ble_hs_util_ensure_addr(0);
    if (error != 0) ESP_LOGW(TAG, "no usable BLE identity address: %d", error);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    if (s_events != NULL) xEventGroupSetBits(s_events, BLUETOOTH__SYNC_DONE_BIT);
}

static void bluetooth__host_task(void *parameter) {
    (void)parameter;
    /* Returns only once nimble_port_stop() is called, which Bruce never does
     * -- the host runs for the lifetime of the process. */
    nimble_port_run();
    nimble_port_freertos_deinit();
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

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        bluetooth__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    error = nimble_port_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE port init failed: %s", esp_err_to_name(error));
        bluetooth__unlock();
        return BRUCE_ERR_IO;
    }

    /* No bonding/pairing store configured: bluetooth__scan_ble() only ever
     * observes advertisements, never connects, so there's nothing to persist.
     * ble_store_read/write() gracefully no-op (BLE_HS_ENOTSUP) without one. */
    ble_hs_cfg.reset_cb = bluetooth__on_reset;
    ble_hs_cfg.sync_cb = bluetooth__on_sync;

    nimble_port_freertos_init(bluetooth__host_task);

    EventBits_t bits = xEventGroupWaitBits(
        s_events, BLUETOOTH__SYNC_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(BLUETOOTH__SYNC_TIMEOUT_MS)
    );
    if ((bits & BLUETOOTH__SYNC_DONE_BIT) == 0) {
        ESP_LOGE(TAG, "NimBLE host/controller sync timed out");
        bluetooth__unlock();
        return BRUCE_ERR_TIMEOUT;
    }

    s_initialized = true;
    bluetooth__unlock();
    return BRUCE_OK;
}

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
    if (s_scanning) {
        bluetooth__unlock();
        return BRUCE_ERR_BUSY;
    }
    size_t scan_capacity = capacity < BLUETOOTH__MAX_RESULTS ? capacity : BLUETOOTH__MAX_RESULTS;
    s_scan_devices = devices;
    s_scan_capacity = scan_capacity;
    s_scan_count = 0;
    s_scanning = true;
    xEventGroupClearBits(s_events, BLUETOOTH__SCAN_DONE_BIT);

    struct ble_gap_disc_params parameters = {
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };
    int error = ble_gap_disc(s_own_addr_type, (int32_t)timeout_ms, &parameters, bluetooth__gap_event, NULL);
    bluetooth__unlock();

    if (error != 0) {
        bluetooth__lock();
        s_scanning = false;
        s_scan_devices = NULL;
        s_scan_capacity = 0;
        s_scan_count = 0;
        bluetooth__unlock();
        return error == BLE_HS_EBUSY ? BRUCE_ERR_BUSY : BRUCE_ERR_IO;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_events, BLUETOOTH__SCAN_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms + 1500)
    );
    if ((bits & BLUETOOTH__SCAN_DONE_BIT) == 0) {
        /* ble_gap_disc()'s own duration should have stopped it by now; this
         * is just a safety net so a stuck scan can't wedge the caller. */
        (void)ble_gap_disc_cancel();
    }

    bluetooth__lock();
    int count = (int)s_scan_count;
    s_scanning = false;
    s_scan_devices = NULL;
    s_scan_capacity = 0;
    s_scan_count = 0;
    bluetooth__unlock();
    if (count > 1) qsort(devices, (size_t)count, sizeof(*devices), bluetooth__compare_rssi);
    return (bits & BLUETOOTH__SCAN_DONE_BIT) != 0 ? count : BRUCE_ERR_TIMEOUT;
}
