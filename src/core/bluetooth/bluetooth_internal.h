#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

bruce_result_t bluetooth__stack_init(void);

/* Testable report translators used by the Classic HID callback. */
void bluetooth_hid__reset_input_state(void);
bruce_result_t bluetooth_hid__translate_keyboard_report(const uint8_t *data, size_t length);
bruce_result_t bluetooth_hid__translate_gamepad_report(const uint8_t *data, size_t length);
