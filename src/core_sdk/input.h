#pragma once

#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/task.h"

typedef enum {
    BRUCE_INPUT_KEY,
    BRUCE_INPUT_BUTTON,
    BRUCE_INPUT_TOUCH,
    BRUCE_INPUT_ENCODER,
    BRUCE_INPUT_CUSTOM,
} bruce_input_type_t;

typedef enum {
    BRUCE_INPUT_PRESS,
    BRUCE_INPUT_RELEASE,
    BRUCE_INPUT_CHANGE,
} bruce_input_action_t;

typedef struct {
    bruce_input_type_t type;
    bruce_input_action_t action;
    int32_t code;
    int32_t value;
    uint64_t timestamp_ms;
    bruce_task_id_t source_task_id;
} bruce_input_event_t;

/* input__read returns BRUCE_OK, BRUCE_ERR_TIMEOUT, BRUCE_ERR_NOT_FOREGROUND,
 * or another BRUCE_ERR_* result.  input__inject returns BRUCE_OK or
 * BRUCE_ERR_PERMISSION/BRUCE_ERR_INVALID_ARGUMENT. */
bruce_result_t input__read(bruce_input_event_t *out_event, uint32_t timeout_ms);
bruce_result_t input__inject(const bruce_input_event_t *event);
