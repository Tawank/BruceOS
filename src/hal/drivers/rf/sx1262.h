#pragma once

#include "hal/hal.h"

/* Semtech SX1262 sub-GHz transceiver on SPI. Pins: "cs" (SoC), "reset",
 * "busy" (SoC input). Rails: its power, on only while in use. */
typedef struct {
    /* SetDIO3AsTCXOCtrl's voltage code (4 is 2.4 V); 0 for a crystal. */
    uint8_t tcxo_code;
    uint16_t tcxo_delay_us;
    bool dio2_rf_switch; /* DIO2 drives the antenna switch. */
} sx1262_config_t;

extern const hal_driver_t sx1262_driver;
