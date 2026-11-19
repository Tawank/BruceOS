#include "input_touch.h"

#include "sdkconfig.h"
#include <stdbool.h>

#if CONFIG_BRUCE_TOUCH_ENABLED

#include <string.h>

#include "input_common.h"
#include "input_hotkey.h"

#include "core_sdk/device.h"
#include "core_sdk/display.h"
#include "core_sdk/pubsub.h"
#include "sdkconfig.h"

/* The touch controller and the board I2C bus it shares with the PMIC are
 * owned exclusively by the device_bus process (see
 * modules/device_bus/) - this module never talks to either
 * directly. It just relays BRUCE_DEVICE_TOPIC_TOUCH messages onto the
 * normal input queue, the same way it would relay physical button presses. */
static bruce_pubsub_id_t s_touch_sub = BRUCE_PUBSUB_ID_INVALID;

/* Touch coordinates match the display at its default rotation; turns the
 * point by however far the display is rotated from that, the same way the
 * display maps its own pixels. */
static void input_touch__rotate(bruce_device_touch_message_t *touch) {
    int turns = (display__get_rotation() - CONFIG_BRUCE_DISPLAY_DEFAULT_ROTATION) & 3;
    bool swapped = CONFIG_BRUCE_DISPLAY_DEFAULT_ROTATION & 1;
    int32_t width = swapped ? CONFIG_BRUCE_DISPLAY_HEIGHT : CONFIG_BRUCE_DISPLAY_WIDTH;
    int32_t height = swapped ? CONFIG_BRUCE_DISPLAY_WIDTH : CONFIG_BRUCE_DISPLAY_HEIGHT;
    int32_t x = touch->x, y = touch->y;
    switch (turns) {
        case 1: touch->x = y; touch->y = width - 1 - x; break;
        case 2: touch->x = width - 1 - x; touch->y = height - 1 - y; break;
        case 3: touch->x = height - 1 - y; touch->y = x; break;
        default: break;
    }
}

/* A touch that starts this close to the top edge and moves down this far runs the "SWIPE_DOWN" hotkey. */
#define INPUT_TOUCH__EDGE_PX 16
#define INPUT_TOUCH__SWIPE_PX 40

static bool s_edge_touch; /* The current touch started at the top edge. */
static bool s_swallow;    /* The swipe fired; the rest of this touch is not the app's. */
static int32_t s_edge_y;

/* True when the touch event is part of a top-edge swipe and must not reach the app. */
static bool input_touch__edge_swipe(const bruce_device_touch_message_t *touch) {
    if (touch->action == BRUCE_INPUT_PRESS) {
        s_edge_touch = touch->y < INPUT_TOUCH__EDGE_PX;
        s_swallow = false;
        s_edge_y = touch->y;
        return false;
    }
    bool swallowed = s_swallow;
    if (touch->action == BRUCE_INPUT_RELEASE) s_edge_touch = s_swallow = false;
    if (swallowed || !s_edge_touch || touch->y - s_edge_y < INPUT_TOUCH__SWIPE_PX) return swallowed;

    uint32_t hold_ms = 0;
    char action[BRUCE_CONFIG_HOTKEY_ACTION_MAX_LEN + 1];
    s_edge_touch = false;
    if (!input_hotkey__find("SWIPE_DOWN", &hold_ms, action, sizeof(action), NULL)) return false;
    /* End the app's touch here, so it doesn't also scroll. */
    bruce_input_event_t release = {
        .type = BRUCE_INPUT_TOUCH, .action = BRUCE_INPUT_RELEASE, .code = BRUCE_INPUT_CODE_TOUCH,
        .x = (int16_t)touch->x, .y = (int16_t)touch->y,
    };
    (void)input__inject(&release);
    s_swallow = touch->action != BRUCE_INPUT_RELEASE;
    input_hotkey__run_action(action);
    return true;
}

/* Touch keys below the screen: process preview, launcher, Back. They run
 * the same actions as the default Ctrl+Tab / Ctrl+Space hotkeys. */
static void input_touch__key_pressed(int32_t key) {
    if (key == 0) {
        input_hotkey__run_action("process preview");
    } else if (key == 1) {
        input_hotkey__run_action("launcher");
    } else if (key == 2) {
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_PRESS, BRUCE_INPUT_CODE_BACK, 1);
        input__emit(BRUCE_INPUT_BUTTON, BRUCE_INPUT_RELEASE, BRUCE_INPUT_CODE_BACK, 0);
    }
}

void input_touch__init(void) { (void)pubsub__subscribe(BRUCE_DEVICE_TOPIC_TOUCH, &s_touch_sub); }

void input_touch__poll(void) {
    if (s_touch_sub == BRUCE_PUBSUB_ID_INVALID) return;

    bruce_pubsub_message_t message;
    while (pubsub__read(s_touch_sub, &message, 0) == BRUCE_OK) {
        if (message.size != sizeof(bruce_device_touch_message_t)) continue;
        bruce_device_touch_message_t touch;
        memcpy(&touch, message.data, sizeof(touch));
        if (touch.key < 0) {
            input_touch__rotate(&touch);
            if (input_touch__edge_swipe(&touch)) continue;
            bruce_input_event_t event = {
                .type = BRUCE_INPUT_TOUCH,
                .action = touch.action,
                .code = BRUCE_INPUT_CODE_TOUCH,
                .x = (int16_t)touch.x,
                .y = (int16_t)touch.y,
            };
            (void)input__inject(&event);
        } else if (touch.action == BRUCE_INPUT_PRESS) {
            input_touch__key_pressed(touch.key);
        }
    }
}

#else

void input_touch__init(void) {}
void input_touch__poll(void) {}

#endif
