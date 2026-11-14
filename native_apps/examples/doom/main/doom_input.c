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

#include <stdbool.h>

#include "core_sdk/input.h"
#include "core_sdk/runtime.h"

#include "doomkeys.h"

/* m_menu.c's boolean; 4 bytes whichever doomtype.h variant compiled it. */
extern unsigned int menuactive;
/* g_game.c's walk/run thrust per tic; auto-walk lowers the walk entry while it holds forward. */
extern int forwardmove[2];

/* 3-button controls (encoder PREV/NEXT + SELECT): encoder detents and timed-bind SELECT arrive as an
 * instant press+release, which Doom would drop inside a single tic, so those become synthesized keys
 * held until a deadline. */
#define DOOM_TURN_HOLD_MS 120u
#define DOOM_TAP_HOLD_MS 80u
#define DOOM_DOUBLE_TAP_MS 400u
/* Auto-walk thrust; Doom's own walk speed is 0x19. */
#define DOOM_AUTO_WALK_SPEED 0x10

typedef struct {
    unsigned char key;
    bool down;
    uint64_t release_at;
} doom_held_key_t;

enum { HELD_TURN_LEFT, HELD_TURN_RIGHT, HELD_FIRE, HELD_USE, HELD_MENU_UP, HELD_MENU_DOWN, HELD_ENTER, HELD_COUNT };

static doom_held_key_t s_held[HELD_COUNT] = {
    [HELD_TURN_LEFT] = {.key = KEY_LEFTARROW},
    [HELD_TURN_RIGHT] = {.key = KEY_RIGHTARROW},
    [HELD_FIRE] = {.key = KEY_FIRE},
    [HELD_USE] = {.key = KEY_USE},
    [HELD_MENU_UP] = {.key = KEY_UPARROW},
    [HELD_MENU_DOWN] = {.key = KEY_DOWNARROW},
    [HELD_ENTER] = {.key = KEY_ENTER},
};

/* Pending synthesized key press, delivered on the next DG_GetKey() call. */
static int s_pending_press = -1;
static bool s_key_nav_down[128];
static bool s_walking;
static bool s_walk_down;
/* A click toggles walking at once; a second click inside the window undoes that and fires instead. */
static uint64_t s_click_ms;
static bool s_click_pending;
static int s_saved_walk_speed;

static void doom_hold(int index, uint32_t hold_ms) {
    doom_held_key_t *held = &s_held[index];
    held->release_at = runtime__now() + hold_ms;
    if (!held->down) s_pending_press = index;
}

/* Returns true when the event was consumed by the 3-button scheme. */
static bool doom_three_button(const bruce_input_event_t *event) {
    bool in_menu = menuactive != 0;
    if (event->code == BRUCE_INPUT_CODE_PREV || event->code == BRUCE_INPUT_CODE_NEXT) {
        if (event->action != BRUCE_INPUT_PRESS) return true;
        bool next = event->code == BRUCE_INPUT_CODE_NEXT;
        if (in_menu) {
            doom_hold(next ? HELD_MENU_DOWN : HELD_MENU_UP, DOOM_TAP_HOLD_MS);
        } else {
            doom_hold(next ? HELD_TURN_RIGHT : HELD_TURN_LEFT, DOOM_TURN_HOLD_MS);
        }
        return true;
    }
    if (event->code != BRUCE_INPUT_CODE_SELECT || event->type != BRUCE_INPUT_BUTTON) return false;
    if (event->action != BRUCE_INPUT_PRESS) return true;
    if (in_menu) {
        doom_hold(HELD_ENTER, DOOM_TAP_HOLD_MS);
        return true;
    }
    uint64_t now = runtime__now();
    s_walking = !s_walking;
    if (!s_click_pending || now - s_click_ms >= DOOM_DOUBLE_TAP_MS) {
        s_click_pending = true;
        s_click_ms = now;
        return true;
    }
    s_click_pending = false;
    doom_hold(HELD_FIRE, DOOM_TAP_HOLD_MS);
    s_held[HELD_USE].release_at = s_held[HELD_FIRE].release_at;
    return true;
}

/* Emits one due synthesized transition, if any. */
static bool doom_synth_key(int *pressed, unsigned char *key) {
    if (s_pending_press >= 0) {
        doom_held_key_t *held = &s_held[s_pending_press];
        s_pending_press = -1;
        held->down = true;
        *pressed = 1;
        *key = held->key;
        return true;
    }
    /* USE rides along with FIRE so a tap also opens doors and flips switches. */
    if (s_held[HELD_FIRE].down && !s_held[HELD_USE].down) {
        s_held[HELD_USE].down = true;
        *pressed = 1;
        *key = KEY_USE;
        return true;
    }
    bool want_walk = s_walking && menuactive == 0;
    if (want_walk != s_walk_down) {
        s_walk_down = want_walk;
        if (want_walk) {
            s_saved_walk_speed = forwardmove[0];
            forwardmove[0] = DOOM_AUTO_WALK_SPEED;
        } else {
            forwardmove[0] = s_saved_walk_speed;
        }
        *pressed = want_walk ? 1 : 0;
        *key = KEY_UPARROW;
        return true;
    }
    uint64_t now = runtime__now();
    for (int i = 0; i < HELD_COUNT; ++i) {
        if (s_held[i].down && now >= s_held[i].release_at) {
            s_held[i].down = false;
            *pressed = 0;
            *key = s_held[i].key;
            return true;
        }
    }
    return false;
}

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
    if (doom_synth_key(pressed, key)) return 1;

    bruce_input_event_t event;
    if (input__poll(&event) != BRUCE_OK) return 0;
    if (event.action != BRUCE_INPUT_PRESS && event.action != BRUCE_INPUT_RELEASE) return 0;
    if (doom_three_button(&event)) return doom_synth_key(pressed, key) ? 1 : DG_GetKey(pressed, key);

    int doom_key;
    if (event.type == BRUCE_INPUT_KEY) {
        int code = event.code & 0x7F;
        /* Keyboard nav keys (ESC, Fn+arrows) press with value -1 but release as their plain character. */
        bool nav = event.action == BRUCE_INPUT_PRESS ? event.value == -1 : s_key_nav_down[code];
        s_key_nav_down[code] = nav && event.action == BRUCE_INPUT_PRESS;
        if (nav) {
            doom_key = bruce_code_to_doom_key(event.code);
        } else if (code == '\n') {
            doom_key = KEY_ENTER;
        } else {
            doom_key = code;
        }
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
