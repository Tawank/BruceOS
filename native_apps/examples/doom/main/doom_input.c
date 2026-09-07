// Bruce platform backend for doomgeneric's DG_GetKey (see
// doomgeneric/doomgeneric.h). doomgeneric/i_input.c's I_GetEvent() drains
// this in a loop each tic and forwards whatever it gets straight into
// Doom's own event queue (D_PostEvent()) as both the key ID and, for
// keydown, the typed character -- see its TranslateKey(), which is a
// pass-through in this vendored copy, not the AT-scancode table its dead
// code suggests. That means the values handed back here need to already be
// in Doom's own vocabulary: either a doomkeys.h KEY_* constant for an
// action key, or a plain ASCII byte for a typed one (BRUCE_INPUT_KEY
// events, e.g. a physical keyboard's letter keys for cheat codes).
//
// Bruce delivers one event per input__poll() call; DG_GetKey() is called in
// a tight loop by I_GetEvent() until it reports no event, so a single
// non-blocking poll per call is the right shape -- it naturally drains
// whatever is queued and then stops.

#include "core_sdk/input.h"

#include "doomkeys.h"

static int bruce_code_to_doom_key(int32_t code) {
    switch (code) {
        case BRUCE_INPUT_CODE_UP: return KEY_UPARROW;
        case BRUCE_INPUT_CODE_DOWN: return KEY_DOWNARROW;
        case BRUCE_INPUT_CODE_LEFT: return KEY_LEFTARROW;
        case BRUCE_INPUT_CODE_RIGHT: return KEY_RIGHTARROW;
        case BRUCE_INPUT_CODE_SELECT: return KEY_ENTER;
        case BRUCE_INPUT_CODE_BACK: return KEY_ESCAPE;
        case BRUCE_INPUT_CODE_MENU: return KEY_ESCAPE;
        case BRUCE_INPUT_CODE_BUTTON_A: return KEY_FIRE;
        case BRUCE_INPUT_CODE_BUTTON_B: return KEY_USE;
        case BRUCE_INPUT_CODE_BUTTON_X: return KEY_TAB;   /* automap */
        case BRUCE_INPUT_CODE_BUTTON_Y: return KEY_ENTER; /* menu confirm */
        case BRUCE_INPUT_CODE_BUTTON_L1: return KEY_STRAFE_L;
        case BRUCE_INPUT_CODE_BUTTON_R1: return KEY_STRAFE_R;
        case BRUCE_INPUT_CODE_BUTTON_L2: return '-'; /* zoom out (automap) */
        case BRUCE_INPUT_CODE_BUTTON_R2: return '='; /* zoom in (automap) */
        case BRUCE_INPUT_CODE_BUTTON_START: return KEY_ENTER;
        case BRUCE_INPUT_CODE_BUTTON_SELECT: return KEY_ESCAPE;
        default: return 0;
    }
}

int DG_GetKey(int *pressed, unsigned char *key) {
    bruce_input_event_t event;
    if (input__poll(&event) != BRUCE_OK) return 0;
    if (event.action != BRUCE_INPUT_PRESS && event.action != BRUCE_INPUT_RELEASE) return 0;

    int doom_key;
    if (event.type == BRUCE_INPUT_KEY) {
        doom_key = event.code & 0x7F;
    } else if (event.type == BRUCE_INPUT_BUTTON) {
        doom_key = bruce_code_to_doom_key(event.code);
    } else {
        return 0;
    }
    if (doom_key == 0) return 0;

    *pressed = event.action == BRUCE_INPUT_PRESS ? 1 : 0;
    *key = (unsigned char)doom_key;
    return 1;
}
