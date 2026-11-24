#pragma once

#include "hal.h"

/* Holds its pins at one level from boot (Linux's GPIO hogs): for chips
 * Bruce has no driver for yet, or that must stay quiet until theirs runs.
 * Pins: any roles. */
typedef struct {
    bool active; /* Every pin active, else inactive (logical levels). */
} hal_hog_config_t;

extern const hal_driver_t hal_hog_driver;
