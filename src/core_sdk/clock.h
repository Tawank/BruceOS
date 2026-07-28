#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

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

/* UTC is kept in the system clock. Local reads apply Config's fixed UTC
 * offset and optional manual one-hour DST adjustment. */
bruce_result_t clock__get_utc(bruce_clock_datetime_t *out);
bruce_result_t clock__get_local(bruce_clock_datetime_t *out);

/* Set the UTC system clock from a local calendar value. Requires `config`. */
bruce_result_t clock__set_local(const bruce_clock_datetime_t *local);

/* Synchronize UTC from pool.ntp.org. Wi-Fi must already be connected.
 * Requires `config` and `wifi`; timeout_ms of zero selects 10 seconds. */
bruce_result_t clock__sync_ntp(uint32_t timeout_ms);
bruce_clock_sync_status_t clock__get_sync_status(void);
const char *clock__get_ntp_server(void);
