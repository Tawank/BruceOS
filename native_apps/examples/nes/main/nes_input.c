#include <stdint.h>

#include "core_sdk/input.h"

#include "noftypes.h"

#include "event.h"
#include "nes/nesinput.h"
#include "nes_input.h" // IWYU pragma: keep
#include "osd.h"

static int s_select_pressed_event;
static int s_select_pending_release;
static uint64_t s_select_press_time_ms;

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

void osd_getinput(void) {
    if (s_select_pending_release != 0) {
        event_t handler = event_get(s_select_pending_release);
        if (handler != NULL) handler(INP_STATE_BREAK);
        s_select_pending_release = 0;
    }

    bruce_input_event_t input;
    while (input__poll(&input) == BRUCE_OK) {
        if (input.action == BRUCE_INPUT_CHANGE) continue;
        int state = input.action == BRUCE_INPUT_PRESS ? INP_STATE_MAKE : INP_STATE_BREAK;
        if (input.code == BRUCE_INPUT_CODE_BACK && state == INP_STATE_MAKE) {
            event_t quit = event_get(event_quit);
            if (quit != NULL) quit(state);
            continue;
        }
        if (input.code == BRUCE_INPUT_CODE_SELECT) {
            int event_code = input.type == BRUCE_INPUT_BUTTON ? event_joypad1_a : event_joypad1_start;
            if (state == INP_STATE_MAKE) {
                event_t handler = event_get(event_code);
                if (handler != NULL) handler(state);
                s_select_pressed_event = event_code;
                s_select_press_time_ms = input.timestamp_ms;
            } else if (s_select_pressed_event == event_code && input.timestamp_ms == s_select_press_time_ms) {
                /* A system action injects its whole tap at once. Give it one
                 * emulation frame while preserving real button hold times. */
                s_select_pending_release = event_code;
                s_select_pressed_event = 0;
            } else {
                event_t handler = event_get(event_code);
                if (handler != NULL) handler(state);
                s_select_pressed_event = 0;
            }
            continue;
        }
        int event_code = event_for_code(input.code);
        event_t handler = event_code != 0 ? event_get(event_code) : NULL;
        if (handler != NULL) handler(state);
    }
}

void osd_getmouse(int *x, int *y, int *button) {
    if (x != NULL) *x = 0;
    if (y != NULL) *y = 0;
    if (button != NULL) *button = 0;
}
