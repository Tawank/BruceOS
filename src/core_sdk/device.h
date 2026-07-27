#pragma once

#include <stdint.h>

#include "core_sdk/result.h"

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} bruce_device_time_t;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
} bruce_device_date_t;

/* Returns battery charge from 0 through 100, or a negative BRUCE_ERR_* value.
 * Analog-only boards estimate charge from battery voltage. */
int device__get_battery(void);

/* Return local time/date using the configured UTC offset and DST setting.
 * BRUCE_ERR_INVALID_STATE means that the system wall clock is not yet valid. */
bruce_result_t device__get_time(bruce_device_time_t *out);
bruce_result_t device__get_date(bruce_device_date_t *out);
