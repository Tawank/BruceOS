#include "hid_keyboard.h"

#include <string.h>

#include "core_sdk/input.h"

static bruce_result_t hid_keyboard__inject(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value) {
    bruce_input_event_t event = {.type = type, .action = action, .code = code, .value = value};
    return input__inject(&event);
}

static int32_t hid_keyboard__code(uint8_t usage, bool shift) {
    if (usage >= 0x04 && usage <= 0x1d) { return (shift ? 'A' : 'a') + usage - 0x04; }
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    if (usage >= 0x1e && usage <= 0x27) {
        return shift ? shifted_digits[usage - 0x1e] : digits[usage - 0x1e];
    }
    switch (usage) {
        case 0x28: return BRUCE_INPUT_CODE_SELECT;
        case 0x29: return BRUCE_INPUT_CODE_BACK;
        case 0x2a: return '\b';
        case 0x2b: return '\t';
        case 0x2c: return ' ';
        case 0x2d: return shift ? '_' : '-';
        case 0x2e: return shift ? '+' : '=';
        case 0x2f: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x4f: return BRUCE_INPUT_CODE_RIGHT;
        case 0x50: return BRUCE_INPUT_CODE_LEFT;
        case 0x51: return BRUCE_INPUT_CODE_DOWN;
        case 0x52: return BRUCE_INPUT_CODE_UP;
        default: return 0;
    }
}

bruce_result_t hid_keyboard__report(hid_keyboard_t *keyboard, const uint8_t *data, size_t length) {
    if (data == NULL || length < 8) return BRUCE_ERR_INVALID_ARGUMENT;
    for (size_t i = 2; i < 8; ++i) {
        if (data[i] == 1) return BRUCE_OK;
    }
    bool shift = (data[0] & 0x22) != 0;
    bruce_result_t result = BRUCE_OK;

    for (size_t old = 0; old < 6; ++old) {
        if (keyboard->usages[old] == 0) continue;
        bool retained = false;
        for (size_t current = 0; current < 6; ++current) {
            if (data[current + 2] == keyboard->usages[old]) retained = true;
        }
        if (!retained && keyboard->codes[old] != 0) {
            bruce_result_t injected =
                hid_keyboard__inject(BRUCE_INPUT_KEY, BRUCE_INPUT_RELEASE, keyboard->codes[old], 0);
            if (result == BRUCE_OK && injected != BRUCE_OK) result = injected;
        }
    }

    uint8_t next_usages[6] = {0};
    int32_t next_codes[6] = {0};
    for (size_t current = 0; current < 6; ++current) {
        uint8_t usage = data[current + 2];
        next_usages[current] = usage;
        if (usage == 0 || usage == 1) continue;
        bool retained = false;
        for (size_t old = 0; old < 6; ++old) {
            if (keyboard->usages[old] == usage) {
                retained = true;
                next_codes[current] = keyboard->codes[old];
                break;
            }
        }
        if (!retained) {
            next_codes[current] = hid_keyboard__code(usage, shift);
            if (next_codes[current] != 0) {
                bruce_result_t injected = hid_keyboard__inject(
                    BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS, next_codes[current], next_codes[current]
                );
                if (result == BRUCE_OK && injected != BRUCE_OK) result = injected;
            }
        }
    }
    memcpy(keyboard->usages, next_usages, sizeof(keyboard->usages));
    memcpy(keyboard->codes, next_codes, sizeof(keyboard->codes));
    return result;
}

void hid_keyboard__release_all(hid_keyboard_t *keyboard) {
    static const uint8_t none[8] = {0};
    (void)hid_keyboard__report(keyboard, none, sizeof(none));
    memset(keyboard, 0, sizeof(*keyboard));
}
