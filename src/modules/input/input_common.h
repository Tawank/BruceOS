#pragma once

#include <stdint.h>

#include "core_sdk/input.h"
#include "sdkconfig.h"

#ifndef INPUT__DEBOUNCE_MS
#ifdef CONFIG_BRUCE_INPUT_DEBOUNCE_MS
#define INPUT__DEBOUNCE_MS CONFIG_BRUCE_INPUT_DEBOUNCE_MS
#else
#define INPUT__DEBOUNCE_MS 30
#endif
#endif

/* Milliseconds since boot, from the FreeRTOS tick count. */
uint64_t input__now_ms(void);

/* Injects one input event onto the shared queue (see core_sdk/input.h).
 * value defaults to code for BRUCE_INPUT_KEY events with value == 0, so
 * character-key callers can pass 0 and get a sane default. */
void input__emit(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value);
