#pragma once

#include "hal.h"

/* A key of the controller's matrix; what it means is the keyboard layout's business. */
typedef struct {
    uint8_t row;
    uint8_t col;
    bool pressed;
} hal_keypad_event_t;

/* Operations of a HAL_CLASS_KEYPAD driver. */
typedef struct {
    /* The next queued key change; BRUCE_ERR_NOT_FOUND when there is none. */
    bruce_result_t (*next_event)(const hal_device_t *device, hal_keypad_event_t *out);
} hal_keypad_api_t;

bruce_result_t hal_keypad_next_event(const hal_device_t *device, hal_keypad_event_t *out);
