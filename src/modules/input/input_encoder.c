#include "input_encoder.h"

#include <stdbool.h>
#include <stdint.h>

#include "input_common.h"

#include "core_sdk/gpio.h"
#include "sdkconfig.h"

#if CONFIG_BRUCE_ENCODER_ENABLED

#define INPUT__ENCODER_PIN_A CONFIG_BRUCE_ENCODER_PIN_A
#define INPUT__ENCODER_PIN_B CONFIG_BRUCE_ENCODER_PIN_B
/* One full detent is 4 quadrature sub-steps on a typical mechanical
 * encoder. */
#define INPUT__ENCODER_STEPS_PER_DETENT 4

/* Standard quadrature gray-code transition table: index is
 * (old_state << 2 | new_state), where each state is (A << 1 | B). Value is
 * -1/0/+1 sub-steps. This table (or an equivalent) shows up across most
 * embedded rotary-encoder decoders (e.g. PJRC's Encoder library, Ben
 * Buxton's Rotary library) - it's the well-known solution, not something
 * specific to any one board here. */
static const int8_t s_encoder_table[16] = {
    0,
    -1,
    1,
    0,
    1,
    0,
    0,
    -1,
    -1,
    0,
    0,
    1,
    0,
    1,
    -1,
    0,
};

static uint8_t s_encoder_prev_state;
static int s_encoder_accum;

static uint8_t input__encoder_read_state(void) {
    int a = 1;
    int b = 1;
    (void)gpio__read(INPUT__ENCODER_PIN_A, &a);
    (void)gpio__read(INPUT__ENCODER_PIN_B, &b);
    return (uint8_t)(((a == 0) << 1) | (b == 0));
}

void input_encoder__init(void) {
    (void)gpio__configure(INPUT__ENCODER_PIN_A, BRUCE_GPIO_MODE_INPUT, BRUCE_GPIO_PULL_UP);
    (void)gpio__configure(INPUT__ENCODER_PIN_B, BRUCE_GPIO_MODE_INPUT, BRUCE_GPIO_PULL_UP);
    s_encoder_prev_state = input__encoder_read_state();
}

void input_encoder__poll(void) {
    uint8_t new_state = input__encoder_read_state();
    int8_t step = s_encoder_table[(s_encoder_prev_state << 2) | new_state];
    s_encoder_prev_state = new_state;
    if (step == 0) return;

    /* Clockwise vs counter-clockwise -> DOWN vs UP is a guess (not verified
     * against real hardware); if a board's encoder feels reversed, swap the
     * two blocks below rather than the A/B pins. */
    s_encoder_accum += step;
    while (s_encoder_accum >= INPUT__ENCODER_STEPS_PER_DETENT) {
        s_encoder_accum -= INPUT__ENCODER_STEPS_PER_DETENT;
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_DOWN, 1);
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, BRUCE_INPUT_CODE_DOWN, 0);
    }
    while (s_encoder_accum <= -INPUT__ENCODER_STEPS_PER_DETENT) {
        s_encoder_accum += INPUT__ENCODER_STEPS_PER_DETENT;
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_UP, 1);
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, BRUCE_INPUT_CODE_UP, 0);
    }
}

#else

void input_encoder__init(void) {}
void input_encoder__poll(void) {}

#endif
