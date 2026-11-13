#include <stdint.h>

#include "core_sdk/input.h"
#include "core_sdk/runtime.h"

#include "noftypes.h"

#include "event.h"
#include "nes/nesinput.h"
#include "nes_input.h" // IWYU pragma: keep
#include "osd.h"

#define NES_INPUT__ENCODER_HOLD_MS 130
#define NES_INPUT__JOYPAD_COUNT (event_joypad1_right - event_joypad1_a + 1)

/* 0 = no release pending; released on the first frame at or after this time. */
static uint64_t s_release_at_ms[NES_INPUT__JOYPAD_COUNT];

static int event_for_code(int32_t code) {
    switch (code) {
        case BRUCE_INPUT_CODE_UP:
        case 'w': return event_joypad1_up;

        case BRUCE_INPUT_CODE_DOWN:
        case 's': return event_joypad1_down;

        case BRUCE_INPUT_CODE_PREV:
        case BRUCE_INPUT_CODE_LEFT:
        case 'a': return event_joypad1_left;

        case BRUCE_INPUT_CODE_NEXT:
        case BRUCE_INPUT_CODE_RIGHT:
        case 'd': return event_joypad1_right;

        case BRUCE_INPUT_CODE_BUTTON_A:
        case 'z':
        case 'j': return event_joypad1_a;

        case BRUCE_INPUT_CODE_BUTTON_B:
        case 'x':
        case 'k': return event_joypad1_b;

        case BRUCE_INPUT_CODE_SELECT:
        case BRUCE_INPUT_CODE_BUTTON_START: return event_joypad1_start;

        case BRUCE_INPUT_CODE_BUTTON_SELECT:
        case ' ': return event_joypad1_select;

        default: return 0;
    }
}

static int event_for_input(const bruce_input_event_t *input) {
    if (input->code == BRUCE_INPUT_CODE_SELECT && input->type == BRUCE_INPUT_BUTTON) return event_joypad1_a;
    return event_for_code(input->code);
}

static void set_joypad(int event_code, int state) {
    event_t handler = event_get(event_code);
    if (handler != NULL) handler(state);
}

static void press(int event_code) {
    s_release_at_ms[event_code - event_joypad1_a] = 0;
    set_joypad(event_code, INP_STATE_MAKE);
}

/* Deferred to a later frame so even an instant tap is seen by the game. */
static void schedule_release(int event_code, uint64_t at_ms) {
    s_release_at_ms[event_code - event_joypad1_a] = at_ms != 0 ? at_ms : 1;
}

static void apply_due_releases(uint64_t now_ms) {
    for (int i = 0; i < NES_INPUT__JOYPAD_COUNT; ++i) {
        if (s_release_at_ms[i] != 0 && s_release_at_ms[i] <= now_ms) {
            s_release_at_ms[i] = 0;
            set_joypad(event_joypad1_a + i, INP_STATE_BREAK);
        }
    }
}

void osd_getinput(void) {
    uint64_t now_ms = runtime__now();
    apply_due_releases(now_ms);

    bruce_input_event_t input;
    while (input__poll(&input) == BRUCE_OK) {
        if (input.action == BRUCE_INPUT_CHANGE) continue;
        if (input.code == BRUCE_INPUT_CODE_BACK && input.action == BRUCE_INPUT_PRESS) {
            set_joypad(event_quit, INP_STATE_MAKE);
            continue;
        }
        int event_code = event_for_input(&input);
        if (event_code == 0) continue;

        if (input.type == BRUCE_INPUT_ENCODER) {
            if (input.action != BRUCE_INPUT_PRESS) continue;
            press(event_code);
            schedule_release(event_code, now_ms + NES_INPUT__ENCODER_HOLD_MS);
        } else if (input.action == BRUCE_INPUT_PRESS) {
            press(event_code);
        } else {
            schedule_release(event_code, input.timestamp_ms);
        }
    }
}

void osd_getmouse(int *x, int *y, int *button) {
    if (x != NULL) *x = 0;
    if (y != NULL) *y = 0;
    if (button != NULL) *button = 0;
}
