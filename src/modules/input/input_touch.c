#include "input_touch.h"

#include <stdbool.h>
#include <string.h>

#include "input_common.h"

#include "core_sdk/device.h"
#include "core_sdk/pubsub.h"
#include "sdkconfig.h"

#if CONFIG_BRUCE_TOUCH_ENABLED

/* The touch controller and the board I2C bus it shares with the PMIC are
 * owned exclusively by the device_bus process (see
 * modules/device_bus/) - this module never talks to either
 * directly. It just relays BRUCE_DEVICE_TOPIC_TOUCH messages onto the
 * normal input queue, the same way it would relay physical button presses. */
static bruce_pubsub_id_t s_touch_sub = BRUCE_PUBSUB_ID_INVALID;

void input_touch__init(void) { (void)pubsub__subscribe(BRUCE_DEVICE_TOPIC_TOUCH, &s_touch_sub); }

void input_touch__poll(void) {
    if (s_touch_sub == BRUCE_PUBSUB_ID_INVALID) return;

    bruce_pubsub_message_t message;
    while (pubsub__read(s_touch_sub, &message, 0) == BRUCE_OK) {
        if (message.size != sizeof(bruce_device_touch_message_t)) continue;
        bruce_device_touch_message_t touch;
        memcpy(&touch, message.data, sizeof(touch));
        input__emit(BRUCE_INPUT_TOUCH, touch.action, touch.x, touch.y);
    }
}

#else

void input_touch__init(void) {}
void input_touch__poll(void) {}

#endif
