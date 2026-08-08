#include "input_buttons.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

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

/* Per-button state.
 *
 * hold_pending/hold_ms/hold_action support the "<N>s <BTN_NAME>" hotkey
 * syntax (see input_hotkey.h): a button with such a binding configured
 * defers its normal press event until the outcome is known -- either the
 * hold threshold is reached (the hotkey action runs instead) or the button
 * is released early (a normal tap is emitted as if there were no binding
 * at all). Buttons with no hold binding are unaffected and behave exactly
 * as before, with zero added latency. hotkey_consumed tracks an *instant*
 * (no duration prefix) hotkey/rebind match, which replaces the press+release
 * pair outright, same as the keyboard's chord hotkeys. */
typedef struct {
    int pin;
    int32_t code;
    bool active_low;
    bool last_stable;
    uint64_t last_event_ms;
    uint64_t press_start_ms;
    bool hold_pending;
    uint32_t hold_ms;
    bool hold_fired;
    bool hotkey_consumed;
    char hold_action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1];
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
        (void)gpio__configure(
            s_buttons[i].pin, BRUCE_GPIO_MODE_INPUT,
            s_buttons[i].active_low ? BRUCE_GPIO_PULL_UP : BRUCE_GPIO_PULL_DOWN
        );
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
            if (now - btn->last_event_ms >= INPUT__DEBOUNCE_MS) {
                btn->last_stable = pressed;
                btn->last_event_ms = now;

                if (pressed) {
                    btn->press_start_ms = now;
                    btn->hold_fired = false;
                    btn->hotkey_consumed = false;

                    const char *name = input_hotkey__name_for_code(btn->code);
                    uint32_t hold_ms = 0;
                    char action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1] = {0};
                    bool matched = name != NULL && input_hotkey__find(name, &hold_ms, action, sizeof(action));

                    if (matched && hold_ms > 0) {
                        /* Hold hotkey: defer the press until we know whether
                         * this becomes a hold or a quick tap. */
                        btn->hold_pending = true;
                        btn->hold_ms = hold_ms;
                        snprintf(btn->hold_action, sizeof(btn->hold_action), "%s", action);
                    } else {
                        btn->hold_pending = false;
                        if (matched) {
                            /* Instant hotkey/rebind: replaces the press+release
                             * pair, same as a keyboard chord hotkey. */
                            input_hotkey__run_action(action);
                            btn->hotkey_consumed = true;
                        } else {
                            input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->code, 1);
                        }
                    }
                } else if (btn->hold_pending) {
                    if (!btn->hold_fired) {
                        /* Released before the hold threshold: behave as if
                         * there were no binding at all. */
                        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->code, 1);
                        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->code, 0);
                    }
                    btn->hold_pending = false;
                } else if (btn->hotkey_consumed) {
                    btn->hotkey_consumed = false;
                } else {
                    input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->code, 0);
                }
            }
        } else if (btn->last_stable && btn->hold_pending && !btn->hold_fired &&
                   now - btn->press_start_ms >= btn->hold_ms) {
            btn->hold_fired = true;
            input_hotkey__run_action(btn->hold_action);
        }
    }
}

#else

void input_buttons__init(void) {}
void input_buttons__poll(void) {}

#endif
