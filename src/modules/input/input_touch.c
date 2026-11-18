#include "input_touch.h"

#include "sdkconfig.h"
#include <stdbool.h>

#if CONFIG_BRUCE_TOUCH_ENABLED

#include <string.h>

#include "input_common.h"
#include "input_hotkey.h"

#include "core_sdk/device.h"
#include "core_sdk/pubsub.h"
#include "sdkconfig.h"

/* The touch controller and the board I2C bus it shares with the PMIC are
 * owned exclusively by the device_bus process (see
 * modules/device_bus/) - this module never talks to either
 * directly. It just relays BRUCE_DEVICE_TOPIC_TOUCH messages onto the
 * normal input queue, the same way it would relay physical button presses. */
static bruce_pubsub_id_t s_touch_sub = BRUCE_PUBSUB_ID_INVALID;

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
