#include "device.h"

#include "core_sdk/device.h"
#include "core_sdk/permission.h"
#include "core_sdk/pubsub.h"  // IWYU pragma: keep
#include "core_sdk/runtime.h" // IWYU pragma: keep

#include <stdbool.h>
#include <string.h> // IWYU pragma: keep

#include "driver/gpio.h"             // IWYU pragma: keep
#include "esp_adc/adc_cali.h"        // IWYU pragma: keep
#include "esp_adc/adc_cali_scheme.h" // IWYU pragma: keep
#include "esp_adc/adc_oneshot.h"     // IWYU pragma: keep
#include "esp_sleep.h"
#include "sdkconfig.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include <stdio.h>
#include <stdlib.h>

#include "esp_core_dump.h"
#include "esp_flash.h"
#endif
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define DEVICE__BATTERY_EMPTY_MV 3300
#define DEVICE__BATTERY_FULL_MV 4150
#define DEVICE__BATTERY_DIVIDER 2
#define DEVICE__BATTERY_SAMPLES 8
#define DEVICE__BATTERY_CACHE_MS 30000
#define DEVICE__VALID_EPOCH_MIN 1577836800

#if CONFIG_BRUCE_QEMU_TEST_MODE || !CONFIG_BRUCE_BATTERY_ENABLED
#define DEVICE__NO_BATTERY 1
#elif CONFIG_BRUCE_BATTERY_BACKEND_AXP2101 || CONFIG_BRUCE_BATTERY_BACKEND_AXP192 ||                         \
    CONFIG_BRUCE_BATTERY_BACKEND_BQ27220
/* All I2C fuel-gauge/PMIC backends are read the same way from here: the
 * device_bus process (modules/device_bus/) is the sole
 * owner of the shared board I2C bus and publishes readings on
 * BRUCE_DEVICE_TOPIC_BATTERY; this file just caches the latest one. */
#define DEVICE__BATTERY_BACKEND_I2C 1
#else
#define DEVICE__BATTERY_CHANNEL ((adc_channel_t)CONFIG_BRUCE_BATTERY_ADC_CHANNEL)
#endif

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void device__restart_task(void *context) {
    uint32_t delay_ms = (uint32_t)(uintptr_t)context;
    if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static void device__power_off_task(void *context) {
    uint32_t delay_ms = (uint32_t)(uintptr_t)context;
    if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
#if CONFIG_BRUCE_POWER_HOLD_GPIO >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_BRUCE_POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)CONFIG_BRUCE_POWER_HOLD_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
#endif
    esp_deep_sleep_start();
}

bruce_result_t device__restart(uint32_t delay_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission != BRUCE_OK) return permission;
    return xTaskCreate(device__restart_task, "device_restart", 2048, (void *)(uintptr_t)delay_ms, 5, NULL) ==
                   pdPASS
               ? BRUCE_OK
               : BRUCE_ERR_NO_MEMORY;
}

bruce_result_t device__power_off(uint32_t delay_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission != BRUCE_OK) return permission;
    return xTaskCreate(device__power_off_task, "device_power_off", 2048, (void *)(uintptr_t)delay_ms, 5, NULL) ==
                   pdPASS
               ? BRUCE_OK
               : BRUCE_ERR_NO_MEMORY;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
/* ESP-IDF reports "nothing saved" several ways (erased partition, bad size,
 * bad checksum); callers only need to know there is no usable dump. */
static bruce_result_t device__coredump_locate(size_t *out_address, size_t *out_size) {
    if (esp_core_dump_image_check() != ESP_OK) return BRUCE_ERR_NOT_FOUND;
    size_t address = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&address, &size) != ESP_OK || size == 0) return BRUCE_ERR_NOT_FOUND;
    *out_address = address;
    *out_size = size;
    return BRUCE_OK;
}
#endif

bruce_result_t device__coredump_get_size(size_t *out_size) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission != BRUCE_OK) return permission;
    if (out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t address = 0;
    return device__coredump_locate(&address, out_size);
#else
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

bruce_result_t device__coredump_get_summary(bruce_device_coredump_summary_t *out_summary) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission != BRUCE_OK) return permission;
    if (out_summary == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t address = 0;
    size_t size = 0;
    bruce_result_t result = device__coredump_locate(&address, &size);
    if (result != BRUCE_OK) return result;

    /* ~300 bytes; heap rather than the caller's (often default-sized) stack. */
    esp_core_dump_summary_t *summary = calloc(1, sizeof(*summary));
    if (summary == NULL) return BRUCE_ERR_NO_MEMORY;
    if (esp_core_dump_get_summary(summary) != ESP_OK) {
        free(summary);
        return BRUCE_ERR_IO;
    }

    memset(out_summary, 0, sizeof(*out_summary));
    snprintf(out_summary->task, sizeof(out_summary->task), "%.*s", (int)sizeof(summary->exc_task), summary->exc_task);
    out_summary->pc = summary->exc_pc;
    uint32_t depth = summary->exc_bt_info.depth;
    if (depth > BRUCE_DEVICE_COREDUMP_BACKTRACE_MAX) depth = BRUCE_DEVICE_COREDUMP_BACKTRACE_MAX;
    for (uint32_t i = 0; i < depth; ++i) out_summary->backtrace[i] = summary->exc_bt_info.bt[i];
    out_summary->backtrace_depth = depth;
    out_summary->backtrace_corrupted = summary->exc_bt_info.corrupted;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    out_summary->exception_cause = summary->ex_info.exc_cause;
    out_summary->exception_address = summary->ex_info.exc_vaddr;
#endif
    snprintf(
        out_summary->app_elf_sha256,
        sizeof(out_summary->app_elf_sha256),
        "%.*s",
        (int)sizeof(summary->app_elf_sha256),
        (const char *)summary->app_elf_sha256
    );
    free(summary);
    if (esp_core_dump_get_panic_reason(out_summary->reason, sizeof(out_summary->reason)) != ESP_OK) {
        out_summary->reason[0] = '\0';
    }
    return BRUCE_OK;
#else
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

bruce_result_t device__coredump_read(size_t offset, void *buffer, size_t size) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission != BRUCE_OK) return permission;
    if (buffer == NULL && size > 0) return BRUCE_ERR_INVALID_ARGUMENT;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t address = 0;
    size_t image_size = 0;
    bruce_result_t result = device__coredump_locate(&address, &image_size);
    if (result != BRUCE_OK) return result;
    if (offset > image_size || size > image_size - offset) return BRUCE_ERR_INVALID_ARGUMENT;
    if (size == 0) return BRUCE_OK;
    return esp_flash_read(NULL, buffer, (uint32_t)(address + offset), (uint32_t)size) == ESP_OK ? BRUCE_OK
                                                                                                : BRUCE_ERR_IO;
#else
    (void)offset;
    (void)size;
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

bruce_result_t device__coredump_erase(void) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission != BRUCE_OK) return permission;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    return esp_core_dump_image_erase() == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
#else
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

void device__power_hold_init(void) {
#if CONFIG_BRUCE_POWER_HOLD_GPIO >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_BRUCE_POWER_HOLD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level((gpio_num_t)CONFIG_BRUCE_POWER_HOLD_GPIO, 1);
#endif
}

static bool device__ensure_lock(void) {
    if (s_lock != NULL) return true;
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) { s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage); }
    portEXIT_CRITICAL(&s_init_mux);
    return s_lock != NULL;
}

#if defined(DEVICE__BATTERY_BACKEND_I2C)

static bruce_pubsub_id_t s_battery_sub = BRUCE_PUBSUB_ID_INVALID;
static bool s_cached_battery_valid;
static int s_cached_battery;

int device__get_battery(void) {
    if (!device__ensure_lock()) return BRUCE_ERR_NO_MEMORY;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_battery_sub == BRUCE_PUBSUB_ID_INVALID &&
        pubsub__subscribe(BRUCE_DEVICE_TOPIC_BATTERY, &s_battery_sub) != BRUCE_OK) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_IO;
    }

    bruce_pubsub_message_t message;
    while (pubsub__read(s_battery_sub, &message, 0) == BRUCE_OK) {
        if (message.size != sizeof(bruce_device_battery_message_t)) continue;
        bruce_device_battery_message_t battery;
        memcpy(&battery, message.data, sizeof(battery));
        s_cached_battery = battery.percent;
        s_cached_battery_valid = true;
    }

    int result = s_cached_battery_valid ? s_cached_battery : BRUCE_ERR_NOT_FOUND;
    xSemaphoreGive(s_lock);
    return result;
}

#elif !defined(DEVICE__NO_BATTERY)
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_battery_initialized;
static bool s_cached_battery_valid;
static int s_cached_battery;
static uint64_t s_battery_read_at;

static bruce_result_t device__init_battery(void) {
    if (s_battery_initialized) return BRUCE_OK;

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_unit_handle_t adc = NULL;
    adc_cali_handle_t cali = NULL;
    if (adc_oneshot_new_unit(&unit_config, &adc) != ESP_OK) return BRUCE_ERR_IO;

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(adc, DEVICE__BATTERY_CHANNEL, &channel_config) != ESP_OK) {
        (void)adc_oneshot_del_unit(adc);
        return BRUCE_ERR_IO;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = DEVICE__BATTERY_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali) != ESP_OK) {
        (void)adc_oneshot_del_unit(adc);
        return BRUCE_ERR_IO;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &cali) != ESP_OK) {
        (void)adc_oneshot_del_unit(adc);
        return BRUCE_ERR_IO;
    }
#else
    (void)adc_oneshot_del_unit(adc);
    return BRUCE_ERR_UNSUPPORTED;
#endif

    s_adc = adc;
    s_adc_cali = cali;
    s_battery_initialized = true;
    return BRUCE_OK;
}

int device__get_battery(void) {
    if (!device__ensure_lock()) return BRUCE_ERR_NO_MEMORY;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    uint64_t now = runtime__now();
    if (s_cached_battery_valid && now - s_battery_read_at < DEVICE__BATTERY_CACHE_MS) {
        int cached = s_cached_battery;
        xSemaphoreGive(s_lock);
        return cached;
    }

    bruce_result_t init = device__init_battery();
    if (init != BRUCE_OK) {
        xSemaphoreGive(s_lock);
        return init;
    }

    int raw_total = 0;
    for (int i = 0; i < DEVICE__BATTERY_SAMPLES; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, DEVICE__BATTERY_CHANNEL, &raw) != ESP_OK) {
            xSemaphoreGive(s_lock);
            return BRUCE_ERR_IO;
        }
        raw_total += raw;
    }

    int pin_mv = 0;
    if (adc_cali_raw_to_voltage(s_adc_cali, raw_total / DEVICE__BATTERY_SAMPLES, &pin_mv) != ESP_OK) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_IO;
    }
    int battery_mv = pin_mv * DEVICE__BATTERY_DIVIDER;
    int percent =
        (battery_mv - DEVICE__BATTERY_EMPTY_MV) * 100 / (DEVICE__BATTERY_FULL_MV - DEVICE__BATTERY_EMPTY_MV);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    s_cached_battery = percent;
    s_cached_battery_valid = true;
    s_battery_read_at = now;
    xSemaphoreGive(s_lock);
    return percent;
}

#else /* DEVICE__NO_BATTERY */

int device__get_battery(void) { return BRUCE_ERR_UNSUPPORTED; }

#endif
