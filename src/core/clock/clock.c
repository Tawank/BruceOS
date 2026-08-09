#include "clock.h"

#include "core/wifi/wifi_common.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/permission.h"
#include "core_sdk/runtime.h"
#include "core_sdk/wifi.h"

#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CLOCK__VALID_EPOCH_MIN 1577836800
#define CLOCK__DEFAULT_SYNC_TIMEOUT_MS 10000u
#define CLOCK__NTP_SERVER "pool.ntp.org"

static StaticSemaphore_t s_sync_lock_storage;
static SemaphoreHandle_t s_sync_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_sync_callback_seen;
static volatile bruce_clock_sync_status_t s_sync_status = BRUCE_CLOCK_SYNC_IDLE;
static volatile bool s_auto_sync_running;

static bool clock__ensure_lock(void) {
    if (s_sync_lock != NULL) return true;
    portENTER_CRITICAL(&s_init_mux);
    if (s_sync_lock == NULL) s_sync_lock = xSemaphoreCreateMutexStatic(&s_sync_lock_storage);
    portEXIT_CRITICAL(&s_init_mux);
    return s_sync_lock != NULL;
}

static bool clock__is_leap_year(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

static bool clock__datetime_valid(const bruce_clock_datetime_t *value) {
    static const uint8_t days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (value == NULL || value->year < 2020 || value->year > 2099 || value->month < 1 || value->month > 12 ||
        value->hour > 23 || value->minute > 59 || value->second > 59) {
        return false;
    }
    uint8_t max_day = days_per_month[value->month - 1];
    if (value->month == 2 && clock__is_leap_year(value->year)) max_day = 29;
    return value->day >= 1 && value->day <= max_day;
}

static time_t clock__timegm(const struct tm *value) {
    int year = value->tm_year + 1900;
    int month = value->tm_mon + 1;
    int64_t days = 0;
    for (int current = 1970; current < year; ++current) days += clock__is_leap_year(current) ? 366 : 365;
    static const uint8_t days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int current = 1; current < month; ++current) {
        days += days_per_month[current - 1];
        if (current == 2 && clock__is_leap_year(year)) days++;
    }
    days += value->tm_mday - 1;
    return (time_t)(days * 86400 + value->tm_hour * 3600 + value->tm_min * 60 + value->tm_sec);
}

static bruce_result_t clock__from_epoch(time_t epoch, bruce_clock_datetime_t *out) {
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (epoch < CLOCK__VALID_EPOCH_MIN) return BRUCE_ERR_INVALID_STATE;
    struct tm value;
    if (gmtime_r(&epoch, &value) == NULL) return BRUCE_ERR_INTERNAL;
    out->year = (uint16_t)(value.tm_year + 1900);
    out->month = (uint8_t)(value.tm_mon + 1);
    out->day = (uint8_t)value.tm_mday;
    out->hour = (uint8_t)value.tm_hour;
    out->minute = (uint8_t)value.tm_min;
    out->second = (uint8_t)value.tm_sec;
    return BRUCE_OK;
}

static time_t clock__local_offset_seconds(void) {
    float timezone = config__get_time_timezone();
    bool dst = config__get_time_dst();
    return (time_t)(timezone * 3600.0f) + (dst ? 3600 : 0);
}

bruce_result_t clock__get_utc(bruce_clock_datetime_t *out) { return clock__from_epoch(time(NULL), out); }

bruce_result_t clock__get_local(bruce_clock_datetime_t *out) {
    return clock__from_epoch(time(NULL) + clock__local_offset_seconds(), out);
}

bruce_result_t clock__set_local(const bruce_clock_datetime_t *local) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_CONFIG);
    if (permission != BRUCE_OK) return permission;
    if (!clock__datetime_valid(local)) return BRUCE_ERR_INVALID_ARGUMENT;
    struct tm value = {
        .tm_sec = local->second,
        .tm_min = local->minute,
        .tm_hour = local->hour,
        .tm_mday = local->day,
        .tm_mon = local->month - 1,
        .tm_year = local->year - 1900,
    };
    time_t utc = clock__timegm(&value) - clock__local_offset_seconds();
    struct timeval tv = {.tv_sec = utc, .tv_usec = 0};
    return settimeofday(&tv, NULL) == 0 ? BRUCE_OK : BRUCE_ERR_IO;
}

static void clock__sntp_callback(struct timeval *tv) {
    (void)tv;
    s_sync_callback_seen = true;
}

static bruce_result_t clock__sync_ntp_internal(uint32_t timeout_ms, bool check_permissions) {
    if (check_permissions) {
        bruce_result_t permission = permission__check(BRUCE_PERMISSION_CONFIG);
        if (permission != BRUCE_OK) return permission;
        permission = permission__check(BRUCE_PERMISSION_WIFI);
        if (permission != BRUCE_OK) return permission;
    }
    if (!(check_permissions ? wifi__is_connected() : wifi__is_connected_internal())) {
        return BRUCE_ERR_INVALID_STATE;
    }
    if (!clock__ensure_lock()) return BRUCE_ERR_NO_MEMORY;
    if (xSemaphoreTake(s_sync_lock, 0) != pdTRUE) return BRUCE_ERR_BUSY;

    if (timeout_ms == 0) timeout_ms = CLOCK__DEFAULT_SYNC_TIMEOUT_MS;
    s_sync_callback_seen = false;
    s_sync_status = BRUCE_CLOCK_SYNC_IN_PROGRESS;
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CLOCK__NTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(clock__sntp_callback);
    esp_sntp_init();

    uint64_t started = runtime__now();
    while (!s_sync_callback_seen && runtime__now() - started < timeout_ms) vTaskDelay(pdMS_TO_TICKS(50));
    esp_sntp_stop();
    bruce_result_t result = s_sync_callback_seen ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
    s_sync_status = result == BRUCE_OK ? BRUCE_CLOCK_SYNC_SUCCEEDED : BRUCE_CLOCK_SYNC_FAILED;
    xSemaphoreGive(s_sync_lock);
    return result;
}

bruce_result_t clock__sync_ntp(uint32_t timeout_ms) { return clock__sync_ntp_internal(timeout_ms, true); }

bruce_clock_sync_status_t clock__get_sync_status(void) { return s_sync_status; }

const char *clock__get_ntp_server(void) { return CLOCK__NTP_SERVER; }

static void clock__auto_sync_task(void *context) {
    (void)context;
    (void)clock__sync_ntp_internal(CLOCK__DEFAULT_SYNC_TIMEOUT_MS, false);
    s_auto_sync_running = false;
    vTaskDelete(NULL);
}

void clock__notify_network_connected(void) {
    if (!config__get_time_automatic_update_via_ntp() || s_auto_sync_running) return;
    s_auto_sync_running = true;
    if (xTaskCreate(clock__auto_sync_task, "clock_ntp", 3072, NULL, 4, NULL) != pdPASS) {
        s_auto_sync_running = false;
        s_sync_status = BRUCE_CLOCK_SYNC_FAILED;
    }
}
