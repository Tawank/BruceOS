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

typedef struct {
    int pin;
    int32_t code;
    bool active_low;
    bool last_stable;
    bool pending;
    bool pending_level;
    uint64_t pending_since_ms;
    uint64_t press_start_ms;
    input_hotkey__tier_t tiers[INPUT__MAX_TIERS];
    size_t tier_count;
    bool fired;
    bool holding;
    int32_t held_code;
} input__button_t;

static input__button_t s_buttons[] = {
#if INPUT__HAS_BUTTON_A
    {.pin = INPUT__PIN_BUTTON_A, .code = INPUT__BUTTON_A_CODE, .active_low = true},
#endif
#if INPUT__HAS_BUTTON_B
    {.pin = INPUT__PIN_BUTTON_B, .code = INPUT__BUTTON_B_CODE, .active_low = true},
#endif
#if INPUT__HAS_BUTTON_C
    {.pin = INPUT__PIN_BUTTON_C, .code = INPUT__BUTTON_C_CODE, .active_low = true},
#endif
#if INPUT__HAS_BUTTON_D
    {.pin = INPUT__PIN_BUTTON_D, .code = INPUT__BUTTON_D_CODE, .active_low = true},
#endif
#if INPUT__HAS_BUTTON_0
    {.pin = INPUT__PIN_BUTTON_0, .code = INPUT__BUTTON_0_CODE, .active_low = true},
#endif
};

static void input_buttons__fire(input__button_t *btn, size_t tier) {
    const char *action = btn->tiers[tier].action;
    btn->fired = true;
    if (action[0] == '\0') {
        btn->held_code = btn->code;
    } else if (!input_hotkey__resolve_bind(action, &btn->held_code)) {
        input_hotkey__run_action(action);
        return;
    }
    btn->holding = true;
    input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, btn->held_code, 1);
}

static void input_buttons__release_at(input__button_t *btn, uint64_t timestamp_ms) {
    if (btn->holding) input__emit_at(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, btn->held_code, 0, timestamp_ms);
    btn->holding = false;
}

static size_t input_buttons__reached_tier(const input__button_t *btn, uint64_t held_ms) {
    size_t tier = 0;
    while (tier + 1 < btn->tier_count && btn->tiers[tier + 1].hold_ms <= held_ms) tier++;
    return tier;
}

static void input_buttons__on_press(input__button_t *btn, uint64_t now) {
    btn->press_start_ms = now;
    btn->fired = false;
    btn->tier_count = input_hotkey__find_tiers(input_hotkey__name_for_code(btn->code), btn->tiers, INPUT__MAX_TIERS);
    if (btn->tier_count == 1) input_buttons__fire(btn, 0);
}

static void input_buttons__on_hold(input__button_t *btn, uint64_t now) {
    if (btn->fired) return;
    size_t last = btn->tier_count - 1;
    if (input_buttons__reached_tier(btn, now - btn->press_start_ms) == last) input_buttons__fire(btn, last);
}

/* A deferred tap is replayed with its real hold time by stamping the release in the future. */
static void input_buttons__on_release(input__button_t *btn, uint64_t now) {
    if (btn->fired) {
        input_buttons__release_at(btn, 0);
        return;
    }
    uint64_t held_ms = now - btn->press_start_ms;
    input_buttons__fire(btn, input_buttons__reached_tier(btn, held_ms));
    input_buttons__release_at(btn, now + held_ms);
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
                    input_buttons__on_press(btn, now);
                } else {
                    input_buttons__on_release(btn, now);
                }
            }
        } else {
            btn->pending = false;
            if (pressed) input_buttons__on_hold(btn, now);
        }
    }
}

#else

void input_buttons__init(void) {}
void input_buttons__poll(void) {}

#endif
