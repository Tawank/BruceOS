#pragma once

#include "hal/hal_touch.h"

/* Hynitron CST3530 / CST66xx touch controller on I2C (usually 0x1A).
 * Pins: "reset" (optional). Config: optional. */
typedef struct {
    hal_touch_orientation_t orientation;
} cst3530_config_t;

extern const hal_driver_t cst3530_driver;
