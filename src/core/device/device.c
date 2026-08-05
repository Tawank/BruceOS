#include "device.h"

#include "board_i2c.h"

#include "core_sdk/device.h"
#include "core_sdk/permission.h"
#include "core_sdk/runtime.h" // IWYU pragma: keep

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"        // IWYU pragma: keep
#include "esp_adc/adc_cali_scheme.h" // IWYU pragma: keep
#include "esp_adc/adc_oneshot.h"     // IWYU pragma: keep
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

/* AXP2101 registers (see e.g. M5Unified's AXP2101_Class.cpp): 0x30 enables
 * the fuel-gauge ADCs, 0xA4 is a direct 0-100 battery percentage once
 * they're running - no voltage-to-percent math needed on this path. */
#define DEVICE__AXP2101_REG_ADC_ENABLE 0x30
#define DEVICE__AXP2101_ADC_ENABLE_MASK 0x0F
#define DEVICE__AXP2101_REG_BATTERY_PERCENT 0xA4

/* AXP192 registers (see M5Unified's AXP192_Class.cpp): 0x82 enables all
 * ADCs (0xFF is the documented "enable everything" value), 0x78/0x79 hold a
 * 12-bit battery voltage reading - raw = (byte0 << 4) | byte1, mV = raw *
 * 1.1. Unlike AXP2101 there's no direct percentage register, so this reuses
 * the same linear empty/full millivolt estimate as the ADC backend below. */
#define DEVICE__AXP192_REG_ADC_ENABLE 0x82
#define DEVICE__AXP192_ADC_ENABLE_ALL 0xFF
#define DEVICE__AXP192_REG_BATTERY_VOLTAGE 0x78
#define DEVICE__AXP192_VOLTAGE_LSB_MV_NUM 11
#define DEVICE__AXP192_VOLTAGE_LSB_MV_DEN 10

#if CONFIG_BRUCE_QEMU_TEST_MODE || !CONFIG_BRUCE_BATTERY_ENABLED
#define DEVICE__NO_BATTERY 1
#elif CONFIG_BRUCE_BATTERY_BACKEND_AXP2101
#define DEVICE__BATTERY_BACKEND_AXP2101 1
#elif CONFIG_BRUCE_BATTERY_BACKEND_AXP192
#define DEVICE__BATTERY_BACKEND_AXP192 1
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

bruce_result_t device__restart(uint32_t delay_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission != BRUCE_OK) return permission;
    return xTaskCreate(device__restart_task, "device_restart", 2048, (void *)(uintptr_t)delay_ms, 5, NULL) == pdPASS
               ? BRUCE_OK
               : BRUCE_ERR_NO_MEMORY;
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

#if defined(DEVICE__BATTERY_BACKEND_AXP2101)

static bool s_battery_initialized;
static bool s_cached_battery_valid;
static int s_cached_battery;
static uint64_t s_battery_read_at;

static bruce_result_t device__init_battery(void) {
    if (s_battery_initialized) return BRUCE_OK;
    i2c_master_bus_handle_t bus = board_i2c__acquire();
    if (bus == NULL) return BRUCE_ERR_IO;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_BATTERY_AXP2101_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_config, &dev) != ESP_OK) return BRUCE_ERR_IO;

    uint8_t enable_adc[] = {DEVICE__AXP2101_REG_ADC_ENABLE, DEVICE__AXP2101_ADC_ENABLE_MASK};
    esp_err_t error = i2c_master_transmit(dev, enable_adc, sizeof(enable_adc), -1);
    (void)i2c_master_bus_rm_device(dev);
    if (error != ESP_OK) return BRUCE_ERR_IO;

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

    i2c_master_bus_handle_t bus = board_i2c__acquire();
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_BATTERY_AXP2101_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (bus == NULL || i2c_master_bus_add_device(bus, &dev_config, &dev) != ESP_OK) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_IO;
    }

    uint8_t reg = DEVICE__AXP2101_REG_BATTERY_PERCENT;
    int8_t raw = 0;
    esp_err_t error = i2c_master_transmit_receive(dev, &reg, 1, (uint8_t *)&raw, 1, -1);
    (void)i2c_master_bus_rm_device(dev);
    if (error != ESP_OK) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_IO;
    }

    int percent = raw;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    s_cached_battery = percent;
    s_cached_battery_valid = true;
    s_battery_read_at = now;
    xSemaphoreGive(s_lock);
    return percent;
}

#elif defined(DEVICE__BATTERY_BACKEND_AXP192)

static bool s_battery_initialized;
static bool s_cached_battery_valid;
static int s_cached_battery;
static uint64_t s_battery_read_at;

static bruce_result_t device__init_battery(void) {
    if (s_battery_initialized) return BRUCE_OK;
    i2c_master_bus_handle_t bus = board_i2c__acquire();
    if (bus == NULL) return BRUCE_ERR_IO;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_BATTERY_AXP192_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_config, &dev) != ESP_OK) return BRUCE_ERR_IO;

    uint8_t enable_adc[] = {DEVICE__AXP192_REG_ADC_ENABLE, DEVICE__AXP192_ADC_ENABLE_ALL};
    esp_err_t error = i2c_master_transmit(dev, enable_adc, sizeof(enable_adc), -1);
    (void)i2c_master_bus_rm_device(dev);
    if (error != ESP_OK) return BRUCE_ERR_IO;

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

    i2c_master_bus_handle_t bus = board_i2c__acquire();
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_BRUCE_BATTERY_AXP192_I2C_ADDR,
        .scl_speed_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (bus == NULL || i2c_master_bus_add_device(bus, &dev_config, &dev) != ESP_OK) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_IO;
    }

    uint8_t reg = DEVICE__AXP192_REG_BATTERY_VOLTAGE;
    uint8_t raw[2] = {0};
    esp_err_t error = i2c_master_transmit_receive(dev, &reg, 1, raw, sizeof(raw), -1);
    (void)i2c_master_bus_rm_device(dev);
    if (error != ESP_OK) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_IO;
    }

    int adc12 = ((int)raw[0] << 4) | (raw[1] & 0x0F);
    int battery_mv = adc12 * DEVICE__AXP192_VOLTAGE_LSB_MV_NUM / DEVICE__AXP192_VOLTAGE_LSB_MV_DEN;
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

#elif !defined(DEVICE__NO_BATTERY)
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_battery_initialized;
static bool s_cached_battery_valid;
static int s_cached_battery;
static uint64_t s_battery_read_at;
#endif

#if !defined(DEVICE__NO_BATTERY) && !defined(DEVICE__BATTERY_BACKEND_AXP2101) &&                                 \
    !defined(DEVICE__BATTERY_BACKEND_AXP192)

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

#elif defined(DEVICE__NO_BATTERY)

int device__get_battery(void) { return BRUCE_ERR_UNSUPPORTED; }

#endif
