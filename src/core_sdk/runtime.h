#pragma once

#include <stdint.h>

#include "core_sdk/result.h"

/* Monotonic milliseconds since boot. Use differences between readings for
 * elapsed-time measurement; this is not wall-clock time. */
uint64_t runtime__now(void);

/* Both return BRUCE_OK once the full duration has elapsed and
 * BRUCE_ERR_CANCELLED if interrupted early. runtime__sleep is interrupted
 * when the calling process is paused (it blocks until resumed, then keeps
 * waiting), stopped, or foregrounded while it was background when the call
 * began. runtime__delay honours pause/stop the same way but is never
 * interrupted merely by foregrounding; it waits its requested duration. */
bruce_result_t runtime__sleep(uint32_t milliseconds);
bruce_result_t runtime__delay(uint32_t milliseconds);
