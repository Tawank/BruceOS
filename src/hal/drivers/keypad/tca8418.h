#pragma once

#include "hal/hal_keypad.h"

/* TI TCA8418 keypad scan controller on I2C (usually 0x34). Pins: "reset" (optional). */
typedef struct {
    uint8_t rows;  /* Matrix rows in use, bit n = ROWn. */
    uint16_t cols; /* Matrix columns in use, bit n = COLn. */
} tca8418_config_t;

extern const hal_driver_t tca8418_driver;
