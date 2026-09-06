#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/input.h"

#include "noftypes.h"

#include "event.h"
#include "nes/nesinput.h"
#include "nes_input.h"
#include "osd.h"

static bool s_enter_start_pending_release;

static int event_for_code(int32_t code) {
    switch (code) {
        case BRUCE_INPUT_CODE_UP: return event_joypad1_up;
        case BRUCE_INPUT_CODE_DOWN: return event_joypad1_down;
        case BRUCE_INPUT_CODE_PREV:
        case BRUCE_INPUT_CODE_LEFT: return event_joypad1_left;
        case BRUCE_INPUT_CODE_NEXT:
        case BRUCE_INPUT_CODE_RIGHT: return event_joypad1_right;
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
        case 'w': return event_joypad1_up;
        case 's': return event_joypad1_down;
        case 'a': return event_joypad1_left;
        case 'd': return event_joypad1_right;
        default: return 0;
    }
}

void osd_getinput(void) {
    if (s_enter_start_pending_release) {
        event_t start = event_get(event_joypad1_start);
        if (start != NULL) start(INP_STATE_BREAK);
        s_enter_start_pending_release = false;
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
            /* Enter can arrive as a press/release pair in one input poll.
             * Nofrendo samples button state after this function returns, so
             * keep Start asserted through the current emulation frame. */
            if (state == INP_STATE_MAKE) {
                event_t start = event_get(event_joypad1_start);
                if (start != NULL) start(state);
                s_enter_start_pending_release = true;
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
