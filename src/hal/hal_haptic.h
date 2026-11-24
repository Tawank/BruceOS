#pragma once

#include "hal.h"

typedef enum {
    HAL_HAPTIC_CLICK,
    HAL_HAPTIC_DOUBLE_CLICK,
    HAL_HAPTIC_BUZZ,
    HAL_HAPTIC_ALERT, /* About 3/4 s, for something that wants attention. */
} hal_haptic_effect_t;

/* Operations of a HAL_CLASS_HAPTIC driver; between hal_device_get() and put(). */
typedef struct {
    bruce_result_t (*play)(const hal_device_t *device, hal_haptic_effect_t effect);
} hal_haptic_api_t;

bruce_result_t hal_haptic_play(const hal_device_t *device, hal_haptic_effect_t effect);
