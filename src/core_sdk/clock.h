#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief System clock reads, local-time conversion, and NTP sync.
 */

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} bruce_clock_datetime_t;

typedef enum {
    BRUCE_CLOCK_SYNC_IDLE = 0,
    BRUCE_CLOCK_SYNC_IN_PROGRESS,
    BRUCE_CLOCK_SYNC_SUCCEEDED,
    BRUCE_CLOCK_SYNC_FAILED,
} bruce_clock_sync_status_t;

/**
 * @brief Reads the system clock, which is kept in UTC.
 *
 * @param out Receives the current UTC date/time.
 */
bruce_result_t clock__get_utc(bruce_clock_datetime_t *out);

/**
 * @brief Reads the system clock converted to local time.
 *
 * Applies Config's fixed UTC offset and optional manual one-hour DST
 * adjustment.
 *
 * @param out Receives the current local date/time.
 */
bruce_result_t clock__get_local(bruce_clock_datetime_t *out);

/**
 * @brief Set the UTC system clock from a local calendar value.
 *
 * @param local Local date/time to convert to UTC and apply.
 * @permission config
 */
bruce_result_t clock__set_local(const bruce_clock_datetime_t *local);

/**
 * @brief Synchronize UTC from pool.ntp.org.
 *
 * Wi-Fi must already be connected.
 *
 * @param timeout_ms Sync timeout in milliseconds, or 0 for 10 seconds.
 * @permission config, wifi
 */
bruce_result_t clock__sync_ntp(uint32_t timeout_ms);

bruce_clock_sync_status_t clock__get_sync_status(void);
const char *clock__get_ntp_server(void);
