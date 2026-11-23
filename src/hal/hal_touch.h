#pragma once

#include "hal.h"

typedef struct {
    bool pressed;
    int32_t x, y; /* Screen pixels, when pressed. */
    int32_t key;  /* A touch key held, or -1. */
} hal_touch_report_t;

/* How the panel sits against the screen (Linux's touchscreen-swapped-x-y,
 * touchscreen-inverted-x/y). Mirroring uses the screen size after the swap. */
typedef struct {
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    uint16_t width;
    uint16_t height;
} hal_touch_orientation_t;

/* Operations of a HAL_CLASS_TOUCH driver. */
typedef struct {
    bruce_result_t (*read)(const hal_device_t *device, hal_touch_report_t *out);
} hal_touch_api_t;

/* Fills `out` (not pressed, no key on any error). */
bruce_result_t hal_touch_read(const hal_device_t *device, hal_touch_report_t *out);

/* For drivers: raw panel coordinates to screen ones. NULL leaves them. */
void hal_touch_orient(const hal_touch_orientation_t *orientation, int32_t *x, int32_t *y);
