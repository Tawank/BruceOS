#include "input_buttons.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "input_common.h"
#include "input_hotkey.h"

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

#define INPUT__MAX_TIERS 3

#if INPUT__HAS_BUTTON_A || INPUT__HAS_BUTTON_B || INPUT__HAS_BUTTON_C || INPUT__HAS_BUTTON_D ||              \
    INPUT__HAS_BUTTON_0

/* tiers: "<N>s <BTN_NAME>" hotkey syntax. The currently active tier fires
 * (and releases) in real time against the actual press/release, instead of
 * being decided only at release -- see input_buttons__activate()'s comment
 * for why. */
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
    size_t active_tier;
    /* Set by input_buttons__activate(): exactly one of these (or neither,
     * for a one-shot command tier) is true at a time. */
    bool bind_active;
    bool raw_active;
    int32_t bind_code;
    input_hotkey__tier_t tiers[INPUT__MAX_TIERS];
    size_t tier_count;
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

/* Fires tier `idx`'s press in real time, the moment it actually applies --
 * a bind mirrors the physical button onto its target, a command just runs
 * once, a plain tap (no action configured) emits the raw code. This is a
 * deliberate departure from waiting until release to decide: that would
 * suppress a short tier's press entirely on a long hold, but it also flattens
 * every real hold down to a zero-duration pulse, which breaks anything that
 * reads how long a button was actually held (e.g. jump height, walking, in
 * an emulator). Promoting live means a very long hold can show both the
 * short tier's effect and the long one's, instead of only the long one -- an
 * accepted tradeoff for keeping real timing intact. */
static void input_buttons__activate(input__button_t *btn, size_t idx) {
    btn->active_tier = idx;
    const char *action = btn->tiers[idx].action;
    btn->bind_active = action[0] != '\0' && input_hotkey__resolve_bind(action, &btn->bind_code);
    btn->raw_active = false;
    if (btn->bind_active) {
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->bind_code, 1);
    } else if (action[0] != '\0') {
        input_hotkey__run_action(action);
    } else {
        btn->raw_active = true;
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->code, 1);
    }
}

/* Releases whatever input_buttons__activate() last pressed; a no-op for a
 * one-shot command tier, which has nothing to release. */
static void input_buttons__deactivate(input__button_t *btn) {
    if (btn->bind_active) {
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->bind_code, 0);
    } else if (btn->raw_active) {
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->code, 0);
    }
    btn->bind_active = false;
    btn->raw_active = false;
}

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
                    const char *name = input_hotkey__name_for_code(btn->code);
                    btn->tier_count = input_hotkey__find_tiers(name, btn->tiers, INPUT__MAX_TIERS);
                    input_buttons__activate(btn, 0);
                } else {
                    input_buttons__deactivate(btn);
                }
            }
        } else {
            btn->pending = false;
            if (btn->last_stable) {
                uint64_t held_ms = now - btn->press_start_ms;
                size_t target = 0;
                for (size_t t = btn->tier_count; t-- > 0;) {
                    if (btn->tiers[t].hold_ms <= held_ms) {
                        target = t;
                        break;
                    }
                }
                if (target > btn->active_tier) {
                    input_buttons__deactivate(btn);
                    input_buttons__activate(btn, target);
                }
            }
        }
    }
}

#else

void input_buttons__init(void) {}
void input_buttons__poll(void) {}

#endif
