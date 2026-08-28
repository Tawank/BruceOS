#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Monotonic time, sleep/delay, periodic timers, and the GUI-requested flag.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t bruce_timer_id_t;

#define BRUCE_TIMER_ID_INVALID ((bruce_timer_id_t)0)

/**
 * @brief Monotonic milliseconds since boot.
 *
 * Use differences between readings for elapsed-time measurement; this is
 * not wall-clock time.
 */
uint64_t runtime__now(void);

/**
 * @brief Sleeps/delays for the given duration.
 *
 * Both return BRUCE_OK once the full duration has elapsed and
 * BRUCE_ERR_CANCELLED if interrupted early. runtime__sleep is interrupted
 * when the calling process is paused (it blocks until resumed, then keeps
 * waiting), stopped, or foregrounded while it was background when the call
 * began. runtime__delay honours pause/stop the same way but is never
 * interrupted merely by foregrounding; it waits its requested duration.
 *
 * @param milliseconds Duration to sleep, in milliseconds.
 */
bruce_result_t runtime__sleep(uint32_t milliseconds);

/**
 * @brief Delays for the given duration, ignoring foregrounding (see runtime__sleep()).
 *
 * @param milliseconds Duration to delay, in milliseconds.
 */
bruce_result_t runtime__delay(uint32_t milliseconds);

/**
 * @brief Starts a process-owned periodic timer that atomically increments `counter`.
 *
 * Every `period_us` microseconds. Timer callbacks execute entirely inside
 * Core and never call application code, so one process's timer cannot
 * block another. The timer is automatically stopped when its owner exits
 * or is killed; callers may stop it early with runtime__timer_stop().
 *
 * @param period_us Tick period in microseconds.
 * @param counter Counter atomically incremented on every tick.
 * @param out_timer_id Receives the new timer's id.
 */
bruce_result_t runtime__timer_start(
    uint32_t period_us, volatile uint32_t *counter, bruce_timer_id_t *out_timer_id
);

/**
 * @brief Waits for the next tick from a timer owned by the calling process.
 *
 * A zero timeout polls; UINT32_MAX waits indefinitely.
 *
 * @param timer_id Timer to wait on.
 * @param timeout_ms Maximum time to wait, in milliseconds (0 polls, UINT32_MAX waits indefinitely).
 */
bruce_result_t runtime__timer_wait(bruce_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Stops a timer started with runtime__timer_start().
 *
 * @param timer_id Timer to stop.
 */
bruce_result_t runtime__timer_stop(bruce_timer_id_t timer_id);

/**
 * @brief Returns true iff the calling process's own "GUI" environment variable is "1".
 *
 * (see core_sdk/environment.h). This is the app-facing self-check;
 * app_runner__environment_requests_gui() is the loader-facing variant used
 * to decide a child's gui_requested before that child's process exists.
 */
bool runtime__gui_requested(void);

#ifdef __cplusplus
}
#endif
