#pragma once

#include "hal/hal.h"

/* TI DRV2605 haptic driver on I2C (0x5A) with an ERM motor, playing effects
 * from its ROM library. Rails: its supply or enable, on only while in use. */
extern const hal_driver_t drv2605_driver;
