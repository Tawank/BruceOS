#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* A keyboard sending HID boot reports (Bluetooth or USB): which keys are down. */
typedef struct {
    uint8_t usages[6];
    int32_t codes[6];
} hid_keyboard_t;

/* Injects the presses and releases between the last report and this 8-byte
 * one: modifiers, reserved, six key usages. */
bruce_result_t hid_keyboard__report(hid_keyboard_t *keyboard, const uint8_t *data, size_t length);

/* Releases every key still down and forgets them. */
void hid_keyboard__release_all(hid_keyboard_t *keyboard);
