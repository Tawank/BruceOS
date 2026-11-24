#pragma once

#include "hal/hal.h"

/* Bosch BHI260AP sensor hub on I2C (0x28 or 0x29). Rails: its supply.
 *
 * Probing checks the product ID, records how the chip boots and powers it
 * off. The first hal_device_get() powers it, uploads Bosch's firmware from
 * /lib/firmware/bhi260ap.fw (or the same path on the SD card) into its RAM,
 * as there is no flash on the boards so far, and boots it. It then stays
 * powered while the ESP runs: idle with its sensors off it draws about
 * 7.5 uA (datasheet), so the next user needs no upload. Reading sensors
 * comes later. */
extern const hal_driver_t bhi260ap_driver;
