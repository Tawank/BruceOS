#pragma once

#include "hal/hal.h"

/* Silergy SY6970 charger on I2C (address 0x6A). Config: none. Leaves the
 * charge settings at the chip's defaults. */
extern const hal_driver_t sy6970_driver;
