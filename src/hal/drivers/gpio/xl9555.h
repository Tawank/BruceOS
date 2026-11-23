#pragma once

#include "hal/hal.h"

/* XINLUDA XL9555, 16 GPIOs on I2C; also sold as TCA9555/PCA9555, same registers. */
typedef struct {
    /* Every pin becomes an output: low for 20 ms at probe (pulsing active-low
     * resets), then these levels. Bit n is pin n, pins 10-17 are bits 8-15. */
    uint16_t levels;
} xl9555_config_t;

extern const hal_driver_t xl9555_driver;
