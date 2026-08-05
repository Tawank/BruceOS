#pragma once

#include <stdint.h>

#include "core_sdk/input.h"
#include "core_sdk/result.h"

/* Returns battery charge from 0 through 100, or a negative BRUCE_ERR_* value.
 * Analog-only boards estimate charge from battery voltage. */
int device__get_battery(void);

/* Schedules a device restart after delay_ms. Requires the `process` permission. */
bruce_result_t device__restart(uint32_t delay_ms);

/* On boards with a touchscreen or an I2C fuel-gauge/PMIC battery backend,
 * the device_bus built-in process owns the shared board I2C bus
 * (see core/device/board_i2c.h) and is the only thing that ever talks to
 * those controllers directly; it publishes their readings on these topics
 * (via core_sdk/pubsub.h) instead. device__get_battery() already reads
 * from the battery topic's cache for you - subscribe to
 * BRUCE_DEVICE_TOPIC_BATTERY yourself only if you need updates as they
 * happen. */
#define BRUCE_DEVICE_TOPIC_TOUCH "device.touch"
#define BRUCE_DEVICE_TOPIC_BATTERY "device.battery"

/* x/y are panel pixel coordinates, already adjusted for the board's
 * configured swap/mirror settings. */
typedef struct {
    bruce_input_action_t action; /* BRUCE_INPUT_PRESS/RELEASE/CHANGE */
    int32_t x;
    int32_t y;
} bruce_device_touch_message_t;

typedef struct {
    int32_t percent; /* 0-100 */
} bruce_device_battery_message_t;
