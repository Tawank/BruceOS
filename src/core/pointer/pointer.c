#include "pointer.h"

#include <stdbool.h>

#include "core/display/display.h"
#include "core/event_loop/event_loop.h"
#include "core_sdk/input.h"
#include "core_sdk/runtime.h"

#define POINTER__W 8
#define POINTER__H 12
#define POINTER__DRAW_MS 30
#define POINTER__DRAW_EPAPER_MS 150
#define POINTER__HIDE_MS 3000
#define POINTER__CLEAR 0xF81F /* Magenta: the pointer never uses it. */
#define POINTER__LEFT 0x01
#define POINTER__RIGHT 0x02
#define POINTER__MIDDLE 0x04

/* An arrow, its tip the hot spot: X black, O white, . see-through. */
static const char *const s_shape[POINTER__H] = {
    "X.......", "XX......", "XOX.....", "XOOX....", "XOOOX...", "XOOOOX..",
    "XOOOOOX.", "XOOOOOOX", "XOOOXXXX", "XOXOX...", "XX.XOX..", "X...XX..",
};

static int s_x = -1, s_y;
static uint8_t s_buttons;
static bool s_moved;       /* Since the pointer was last drawn. */
static uint64_t s_drawn_at;
static uint64_t s_moved_at;
static bruce_display_overlay_id_t s_overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
static bool s_shown;

static void pointer__inject(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value) {
    bruce_input_event_t event = {.type = type, .action = action, .code = code, .value = value};
    if (type == BRUCE_INPUT_TOUCH) {
        event.x = (int16_t)s_x;
        event.y = (int16_t)s_y;
    }
    (void)input__inject(&event);
}

static void pointer__button(bool down, int32_t code) {
    pointer__inject(BRUCE_INPUT_BUTTON, down ? BRUCE_INPUT_PRESS : BRUCE_INPUT_RELEASE, code, down);
}

static int pointer__clamp(int value, int limit) { return value < 0 ? 0 : value >= limit ? limit - 1 : value; }

void pointer__report(uint8_t buttons, int dx, int dy, int wheel) {
    int width = display__screen_width(), height = display__screen_height();
    if (width <= 0 || height <= 0) return;
    if (s_x < 0) {
        s_x = width / 2;
        s_y = height / 2;
    }
    if (dx != 0 || dy != 0) {
        s_x = pointer__clamp(s_x + dx, width);
        s_y = pointer__clamp(s_y + dy, height);
        s_moved = true;
        s_moved_at = runtime__now();
        event_loop__note_input(); /* Moving it keeps the screen up, as touching does. */
    }
    uint8_t changed = buttons ^ s_buttons;
    s_buttons = buttons;
    if (changed & POINTER__LEFT) {
        pointer__inject(BRUCE_INPUT_TOUCH, (buttons & POINTER__LEFT) ? BRUCE_INPUT_PRESS : BRUCE_INPUT_RELEASE,
                        BRUCE_INPUT_CODE_TOUCH, 0);
    }
    if (changed & POINTER__RIGHT) pointer__button(buttons & POINTER__RIGHT, BRUCE_INPUT_CODE_BACK);
    if (changed & POINTER__MIDDLE) pointer__button(buttons & POINTER__MIDDLE, BRUCE_INPUT_CODE_SELECT);
    for (; wheel > 0; --wheel) {
        pointer__button(true, BRUCE_INPUT_CODE_UP);
        pointer__button(false, BRUCE_INPUT_CODE_UP);
    }
    for (; wheel < 0; ++wheel) {
        pointer__button(true, BRUCE_INPUT_CODE_DOWN);
        pointer__button(false, BRUCE_INPUT_CODE_DOWN);
    }
}

/* The overlay sits inside the screen; at the right or bottom edge the arrow
 * stops short while the point it touches keeps going. */
static void pointer__place(void) {
    int x = s_x, y = s_y;
    int width = display__screen_width(), height = display__screen_height();
    if (x > width - POINTER__W) x = width - POINTER__W;
    if (y > height - POINTER__H) y = height - POINTER__H;
    if (s_overlay == BRUCE_DISPLAY_OVERLAY_ID_INVALID) {
        bruce_display_color_t pixels[POINTER__W * POINTER__H];
        for (int row = 0; row < POINTER__H; ++row) {
            for (int col = 0; col < POINTER__W; ++col) {
                char c = s_shape[row][col];
                pixels[row * POINTER__W + col] = c == 'X' ? 0x0000 : c == 'O' ? 0xFFFF : POINTER__CLEAR;
            }
        }
        if (display__overlay_create((int16_t)x, (int16_t)y, POINTER__W, POINTER__H, &s_overlay) != BRUCE_OK) {
            s_overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
            return;
        }
        (void)display__overlay_set_image(s_overlay, pixels, POINTER__CLEAR);
    } else {
        (void)display__overlay_move(s_overlay, (int16_t)x, (int16_t)y);
    }
    if (!s_shown) s_shown = display__overlay_show(s_overlay) == BRUCE_OK;
}

void pointer__poll(void) {
    uint64_t now = runtime__now();
    uint32_t every = display__is_epaper() ? POINTER__DRAW_EPAPER_MS : POINTER__DRAW_MS;
    if (s_moved && now - s_drawn_at >= every) {
        s_moved = false;
        s_drawn_at = now;
        pointer__place();
        if (s_buttons & POINTER__LEFT) pointer__inject(BRUCE_INPUT_TOUCH, BRUCE_INPUT_CHANGE, BRUCE_INPUT_CODE_TOUCH, 0);
    }
    if (s_shown && !s_moved && !(s_buttons & POINTER__LEFT) && now - s_moved_at >= POINTER__HIDE_MS) {
        s_shown = display__overlay_hide(s_overlay) != BRUCE_OK;
    }
}

void pointer__end(void) {
    if (s_buttons & POINTER__LEFT) pointer__inject(BRUCE_INPUT_TOUCH, BRUCE_INPUT_RELEASE, BRUCE_INPUT_CODE_TOUCH, 0);
    if (s_buttons & POINTER__RIGHT) pointer__button(false, BRUCE_INPUT_CODE_BACK);
    if (s_buttons & POINTER__MIDDLE) pointer__button(false, BRUCE_INPUT_CODE_SELECT);
    if (s_overlay != BRUCE_DISPLAY_OVERLAY_ID_INVALID) (void)display__overlay_destroy(s_overlay);
    s_overlay = BRUCE_DISPLAY_OVERLAY_ID_INVALID;
    s_shown = false;
    s_buttons = 0;
    s_moved = false;
}
