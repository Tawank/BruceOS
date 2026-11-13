#include "input_buttons.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "input_common.h"
#include "input_hotkey.h"

#include "core_sdk/config.h"
#include "core_sdk/gpio.h"
#include "sdkconfig.h"

#define INPUT__HAS_BUTTON_0 CONFIG_BRUCE_BUTTON_SELECT_ENABLED
#if CONFIG_BRUCE_BUTTON_SELECT_ENABLED
#define INPUT__PIN_BUTTON_0 CONFIG_BRUCE_BUTTON_SELECT_GPIO
#define INPUT__BUTTON_0_CODE BRUCE_INPUT_CODE_SELECT
#endif

#define INPUT__HAS_BUTTON_A CONFIG_BRUCE_BUTTON_A_ENABLED
#if CONFIG_BRUCE_BUTTON_A_ENABLED
#define INPUT__PIN_BUTTON_A CONFIG_BRUCE_BUTTON_A_GPIO
#define INPUT__BUTTON_A_CODE BRUCE_INPUT_CODE_BUTTON_A
#endif

#define INPUT__HAS_BUTTON_B CONFIG_BRUCE_BUTTON_B_ENABLED
#if CONFIG_BRUCE_BUTTON_B_ENABLED
#define INPUT__PIN_BUTTON_B CONFIG_BRUCE_BUTTON_B_GPIO
#define INPUT__BUTTON_B_CODE BRUCE_INPUT_CODE_BUTTON_B
#endif

#define INPUT__HAS_BUTTON_C CONFIG_BRUCE_BUTTON_C_ENABLED
#if CONFIG_BRUCE_BUTTON_C_ENABLED
#define INPUT__PIN_BUTTON_C CONFIG_BRUCE_BUTTON_C_GPIO
#define INPUT__BUTTON_C_CODE BRUCE_INPUT_CODE_BUTTON_C
#endif

#define INPUT__HAS_BUTTON_D CONFIG_BRUCE_BUTTON_D_ENABLED
#if CONFIG_BRUCE_BUTTON_D_ENABLED
#define INPUT__PIN_BUTTON_D CONFIG_BRUCE_BUTTON_D_GPIO
#define INPUT__BUTTON_D_CODE BRUCE_INPUT_CODE_BUTTON_X
#endif

#if INPUT__HAS_BUTTON_A || INPUT__HAS_BUTTON_B || INPUT__HAS_BUTTON_C || INPUT__HAS_BUTTON_D ||              \
    INPUT__HAS_BUTTON_0

/* hold_pending/hold_ms/hold_action: "<N>s <BTN_NAME>" hotkey syntax. */
typedef struct {
    int pin;
    int32_t code;
    bool active_low;
    bool last_stable;
    /* Confirm-then-commit debounce: a reading opposite last_stable must
     * read the same way for a full INPUT__DEBOUNCE_MS before it commits. */
    bool pending;
    bool pending_level;
    uint64_t pending_since_ms;
    uint64_t press_start_ms;
    bool hold_pending;
    uint32_t hold_ms;
    bool hold_fired;
    bool hotkey_consumed;
    /* Set when this press matched a "bind <NAME>" instead of a command. */
    bool bind_active;
    int32_t bind_code;
    char hold_action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1];
    char instant_action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1];
} input__button_t;

static input__button_t s_buttons[] = {
#if INPUT__HAS_BUTTON_A
    {.pin = INPUT__PIN_BUTTON_A, .code = INPUT__BUTTON_A_CODE, .active_low = true, .last_stable = true},
#endif
#if INPUT__HAS_BUTTON_B
    {.pin = INPUT__PIN_BUTTON_B, .code = INPUT__BUTTON_B_CODE, .active_low = true, .last_stable = true},
#endif
#if INPUT__HAS_BUTTON_C
    {.pin = INPUT__PIN_BUTTON_C, .code = INPUT__BUTTON_C_CODE, .active_low = true, .last_stable = true},
#endif
#if INPUT__HAS_BUTTON_D
    {.pin = INPUT__PIN_BUTTON_D, .code = INPUT__BUTTON_D_CODE, .active_low = true, .last_stable = true},
#endif
#if INPUT__HAS_BUTTON_0
    {.pin = INPUT__PIN_BUTTON_0, .code = INPUT__BUTTON_0_CODE, .active_low = true, .last_stable = true},
#endif
};

void input_buttons__init(void) {
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        bruce_gpio_pull_t pull = s_buttons[i].active_low ? BRUCE_GPIO_PULL_UP : BRUCE_GPIO_PULL_DOWN;
#if CONFIG_IDF_TARGET_ESP32
        /* GPIO34-39 are input-only pads with no internal pull resistors. */
        if (s_buttons[i].pin >= 34) pull = BRUCE_GPIO_PULL_NONE;
#endif
        (void)gpio__configure(s_buttons[i].pin, BRUCE_GPIO_MODE_INPUT, pull);
    }
}

void input_buttons__poll(void) {
    uint64_t now = input__now_ms();
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        input__button_t *btn = &s_buttons[i];
        int level = btn->active_low ? 1 : 0;
        (void)gpio__read(btn->pin, &level);
        bool raw = level == 0;
        bool pressed = btn->active_low ? raw : !raw;

        if (pressed != btn->last_stable) {
            if (!btn->pending || btn->pending_level != pressed) {
                btn->pending = true;
                btn->pending_level = pressed;
                btn->pending_since_ms = now;
            } else if (now - btn->pending_since_ms >= INPUT__DEBOUNCE_MS) {
                btn->pending = false;
                btn->last_stable = pressed;

                if (pressed) {
                    btn->press_start_ms = now;
                    btn->hold_fired = false;
                    btn->hotkey_consumed = false;

                    const char *name = input_hotkey__name_for_code(btn->code);
                    uint32_t hold_ms = 0;
                    uint32_t instant_hold_ms = 0;
                    char hold_action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1] = {0};
                    btn->instant_action[0] = '\0';
                    bool hold_matched = name != NULL && input_hotkey__find_by_hold(
                                                               name, true, &hold_ms, hold_action, sizeof(hold_action)
                                                           );
                    bool instant_matched = name != NULL && input_hotkey__find_by_hold(
                                                                  name,
                                                                  false,
                                                                  &instant_hold_ms,
                                                                  btn->instant_action,
                                                                  sizeof(btn->instant_action)
                                                              );

                    if (hold_matched) {
                        btn->hold_pending = true;
                        btn->hold_ms = hold_ms;
                        snprintf(btn->hold_action, sizeof(btn->hold_action), "%s", hold_action);
                    } else {
                        btn->hold_pending = false;
                        btn->bind_active = instant_matched &&
                                            input_hotkey__resolve_bind(btn->instant_action, &btn->bind_code);
                        if (btn->bind_active) {
                            input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->bind_code, 1);
                        } else if (instant_matched) {
                            input_hotkey__run_action(btn->instant_action);
                            btn->hotkey_consumed = true;
                        } else {
                            input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->code, 1);
                        }
                    }
                } else if (btn->hold_pending) {
                    if (!btn->hold_fired) {
                        int32_t bind_code;
                        if (btn->instant_action[0] != '\0' &&
                            input_hotkey__resolve_bind(btn->instant_action, &bind_code)) {
                            input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, bind_code, 1);
                            input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, bind_code, 0);
                        } else {
                            input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->code, 1);
                            input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->code, 0);
                            if (btn->instant_action[0] != '\0') input_hotkey__run_action(btn->instant_action);
                        }
                    }
                    btn->hold_pending = false;
                    btn->instant_action[0] = '\0';
                } else if (btn->bind_active) {
                    input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->bind_code, 0);
                    btn->bind_active = false;
                } else if (btn->hotkey_consumed) {
                    btn->hotkey_consumed = false;
                } else {
                    input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->code, 0);
                }
            }
        } else {
            btn->pending = false;
            if (btn->last_stable && btn->hold_pending && !btn->hold_fired &&
                now - btn->press_start_ms >= btn->hold_ms) {
                btn->hold_fired = true;
                input_hotkey__run_action(btn->hold_action);
            }
        }
    }
}

#else

void input_buttons__init(void) {}
void input_buttons__poll(void) {}

#endif
