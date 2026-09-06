#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Clock, time zone, and NTP settings.
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
 * @brief Sets the system clock from a local date and time.
 *
 * @param local Local date/time to convert to UTC and apply.
 * @permission config
 */
bruce_result_t clock__set_local(const bruce_clock_datetime_t *local);

/**
 * @brief Synchronizes the clock over the network.
 *
 * Wi-Fi must already be connected.
 *
 * @param timeout_ms Sync timeout in milliseconds, or 0 for 10 seconds.
 * @permission config, wifi
 */
bruce_result_t clock__sync_ntp(uint32_t timeout_ms);

/** @brief Returns the status of the latest clock synchronization. */
bruce_clock_sync_status_t clock__get_sync_status(void);
/** @brief Returns the configured NTP server address. */
const char *clock__get_ntp_server(void);

/**
 * @brief Converts a date/time to a Unix epoch timestamp.
 *
 * Pure calendar math: the fields are taken as UTC and neither reads nor
 * depends on the system clock, unlike clock__set_local(). month must be in
 * [1, 12]; callers with a denormalized month (as from struct tm arithmetic)
 * must fold it into year themselves first.
 *
 * @param value Date/time to convert.
 * @param out_epoch Receives the corresponding Unix epoch seconds.
 */
bruce_result_t clock__datetime_to_epoch(const bruce_clock_datetime_t *value, int64_t *out_epoch);

/**
 * @brief Converts a Unix epoch timestamp to a date/time.
 *
 * Pure calendar math, treating epoch as UTC: neither reads nor depends on
 * the system clock, and -- unlike clock__get_utc() -- accepts any epoch
 * value, not just one that looks like a plausibly-synced current time.
 *
 * @param epoch Unix epoch seconds.
 * @param out Receives the corresponding UTC date/time.
 */
bruce_result_t clock__epoch_to_datetime(int64_t epoch, bruce_clock_datetime_t *out);

/**
 * @brief Returns Config's currently configured local-time offset from UTC, in seconds.
 *
 * Combines the fixed UTC-offset timezone setting with the optional manual
 * one-hour DST adjustment -- the same inputs clock__get_local() itself
 * applies.
 */
int64_t clock__get_local_offset_seconds(void);
