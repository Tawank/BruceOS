#pragma once

#include <stdint.h>

#include "core_sdk/result.h"

/* Returns battery charge from 0 through 100, or a negative BRUCE_ERR_* value.
 * Analog-only boards estimate charge from battery voltage. */
int device__get_battery(void);

/* Schedules a device restart after delay_ms. Requires the `process` permission. */
bruce_result_t device__restart(uint32_t delay_ms);
