#include "device.h"

#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/task.h"

#include <stdbool.h>
#include <time.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DEVICE__BATTERY_EMPTY_MV 3300
#define DEVICE__BATTERY_FULL_MV 4150
#define DEVICE__BATTERY_DIVIDER 2
#define DEVICE__BATTERY_SAMPLES 8
#define DEVICE__BATTERY_CACHE_MS 30000
#define DEVICE__VALID_EPOCH_MIN 1577836800

#if defined(CONFIG_BRUCE_BOARD_M5_CARDPUTER)
#    define DEVICE__BATTERY_CHANNEL ADC_CHANNEL_9
#elif defined(CONFIG_BRUCE_BOARD_M5_STICKC_PLUS2)
#    define DEVICE__BATTERY_CHANNEL ADC_CHANNEL_2
#else
#    define DEVICE__NO_BATTERY 1
#endif

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_battery_initialized;
static bool s_cached_battery_valid;
static int s_cached_battery;
static uint64_t s_battery_read_at;

static bool device__ensure_lock(void)
{
    if (s_lock != NULL) return true;
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    portEXIT_CRITICAL(&s_init_mux);
    return s_lock != NULL;
}

static bruce_result_t device__init_battery(void)
{
#if defined(DEVICE__NO_BATTERY)
    return BRUCE_ERR_UNSUPPORTED;
#else
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
#endif
}

int device__get_battery(void)
{
#if defined(DEVICE__NO_BATTERY)
    return BRUCE_ERR_UNSUPPORTED;
#else
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
    int percent = (battery_mv - DEVICE__BATTERY_EMPTY_MV) * 100 /
                  (DEVICE__BATTERY_FULL_MV - DEVICE__BATTERY_EMPTY_MV);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    s_cached_battery = percent;
    s_cached_battery_valid = true;
    s_battery_read_at = now;
    xSemaphoreGive(s_lock);
    return percent;
#endif
}

static bruce_result_t device__get_local_datetime(struct tm *out)
{
    time_t now = time(NULL);
    if (now < DEVICE__VALID_EPOCH_MIN) return BRUCE_ERR_INVALID_STATE;

    float timezone = 0.0f;
    bool dst = false;
    (void)config__get_tmz(&timezone);
    (void)config__get_dst(&dst);
    now += (time_t)(timezone * 3600.0f) + (dst ? 3600 : 0);
    return gmtime_r(&now, out) != NULL ? BRUCE_OK : BRUCE_ERR_INTERNAL;
}

bruce_result_t device__get_time(bruce_device_time_t *out)
{
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    struct tm local;
    bruce_result_t result = device__get_local_datetime(&local);
    if (result != BRUCE_OK) return result;
    out->hour = (uint8_t)local.tm_hour;
    out->minute = (uint8_t)local.tm_min;
    out->second = (uint8_t)local.tm_sec;
    return BRUCE_OK;
}

bruce_result_t device__get_date(bruce_device_date_t *out)
{
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    struct tm local;
    bruce_result_t result = device__get_local_datetime(&local);
    if (result != BRUCE_OK) return result;
    out->year = (uint16_t)(local.tm_year + 1900);
    out->month = (uint8_t)(local.tm_mon + 1);
    out->day = (uint8_t)local.tm_mday;
    return BRUCE_OK;
}
