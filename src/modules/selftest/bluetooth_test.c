#include "bluetooth_test.h"

#include <stdio.h>

#include "core/bluetooth/bluetooth_internal.h"
#include "core_sdk/input.h"

bool selftest__run_bluetooth_hid_keyboard_translation_case(void) {
    (void)input__flush();
    bluetooth_hid__reset_input_state();
    const uint8_t press[8] = {0x02, 0, 0x04, 0, 0, 0, 0, 0};
    const uint8_t release[8] = {0};
    if (bluetooth_hid__translate_keyboard_report(press, sizeof(press)) != BRUCE_OK ||
        bluetooth_hid__translate_keyboard_report(release, sizeof(release)) != BRUCE_OK) {
        printf("[selftest] bluetooth-hid/keyboard: FAIL, translation error\n");
        return false;
    }
    bruce_input_event_t event;
    bool press_ok = input__read(&event, 0) == BRUCE_OK && event.type == BRUCE_INPUT_KEY &&
                    event.action == BRUCE_INPUT_PRESS && event.code == 'A';
    bool release_ok = input__read(&event, 0) == BRUCE_OK && event.type == BRUCE_INPUT_KEY &&
                      event.action == BRUCE_INPUT_RELEASE && event.code == 'A';
    bool ok = press_ok && release_ok;
    printf("[selftest] bluetooth-hid/keyboard: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_bluetooth_hid_validation_case(void) {
    uint8_t short_report[2] = {0};
    bool ok = bluetooth_hid__translate_keyboard_report(NULL, 8) == BRUCE_ERR_INVALID_ARGUMENT &&
              bluetooth_hid__translate_keyboard_report(short_report, sizeof(short_report)) ==
                  BRUCE_ERR_INVALID_ARGUMENT &&
              bluetooth_hid__translate_gamepad_report(NULL, 7) == BRUCE_ERR_INVALID_ARGUMENT &&
              bluetooth_hid__translate_gamepad_report(short_report, sizeof(short_report)) ==
                  BRUCE_ERR_INVALID_ARGUMENT;
    printf("[selftest] bluetooth-hid/validation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
