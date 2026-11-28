#pragma once

#include "hal/hal.h"

/* TI CC1101 sub-GHz transceiver on SPI. Pins: "cs" (SoC; the driver drives
 * it for the power-on reset sequence). Rails: its power, on only while in use. */
extern const hal_driver_t cc1101_driver;
