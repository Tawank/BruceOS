#pragma once

#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/task.h"

typedef enum {
    BRUCE_INPUT_KEY,
    BRUCE_INPUT_BUTTON,
    BRUCE_INPUT_TOUCH,
    BRUCE_INPUT_ENCODER,
    BRUCE_INPUT_CUSTOM,
} bruce_input_type_t;

typedef enum {
    BRUCE_INPUT_PRESS,
    BRUCE_INPUT_RELEASE,
    BRUCE_INPUT_CHANGE,
} bruce_input_action_t;

/*
 * Common input codes.
 *
 * For BRUCE_INPUT_KEY events the code is normally the ASCII value of the
 * character (0x00-0x7F).  Navigation and action codes are defined below so that
 * physical buttons and Fn-chord keyboard bindings can emit the same
 * semantic events.
 */
#define BRUCE_INPUT_CODE_UP      0x100
#define BRUCE_INPUT_CODE_DOWN    0x101
#define BRUCE_INPUT_CODE_LEFT    0x102
#define BRUCE_INPUT_CODE_RIGHT   0x103
#define BRUCE_INPUT_CODE_SELECT  0x104
#define BRUCE_INPUT_CODE_BACK    0x105
#define BRUCE_INPUT_CODE_MENU    0x106
#define BRUCE_INPUT_CODE_HOME    0x107
#define BRUCE_INPUT_CODE_BUTTON_A 0x200
#define BRUCE_INPUT_CODE_BUTTON_B 0x201
#define BRUCE_INPUT_CODE_BUTTON_C 0x202

typedef struct {
    bruce_input_type_t type;
    bruce_input_action_t action;
    int32_t code;
    int32_t value;
    uint64_t timestamp_ms;
    bruce_task_id_t source_task_id;
} bruce_input_event_t;

/* input__read pops the next input event for the foreground task.
 *
 * `timeout_ms` controls blocking:
 *   - 0      : non-blocking; returns BRUCE_ERR_TIMEOUT immediately if no event
 *             is queued.
 *   - >0     : blocks up to the requested total elapsed milliseconds.
 *   - 0xFFFFFFFF (portMAX_DELAY equivalent) : block until an event arrives.
 *
 * Returns BRUCE_OK with *out_event filled, BRUCE_ERR_TIMEOUT if no event
 * arrived within the timeout, or BRUCE_ERR_NOT_FOREGROUND if the caller is not
 * the current foreground task. A blocked read is revoked immediately when its
 * foreground tenure ends; regaining foreground requires a new call. An
 * indefinite read is also interrupted by input deinitialization.
 *
 * Physical input is delivered only to the foreground task; background tasks
 * must call task__foreground() or use input__inject() (which requires the
 * `input` permission).
 */
bruce_result_t input__read(bruce_input_event_t *out_event, uint32_t timeout_ms);

/* input__poll is a convenience wrapper for a non-blocking read.  It returns
 * BRUCE_OK with *out_event filled, or BRUCE_ERR_TIMEOUT if no event is
 * available.  It is equivalent to input__read(out_event, 0).
 *
 * Typical polling loop:
 *   bruce_input_event_t ev;
 *   while (input__poll(&ev) == BRUCE_OK) { ... handle ev ... }
 */
static inline bruce_result_t input__poll(bruce_input_event_t *out_event)
{
    return input__read(out_event, 0);
}

/* input__flush removes all queued input events.  It is useful when switching
 * screens so that stale presses do not affect the new UI.  Returns BRUCE_OK
 * or BRUCE_ERR_NOT_FOREGROUND if the caller is not the foreground task. */
bruce_result_t input__flush(void);

/* input__peek inspects the next queued event without removing it.
 *
 * Returns BRUCE_OK with *out_event filled, BRUCE_ERR_TIMEOUT if the queue is
 * empty, or BRUCE_ERR_NOT_FOREGROUND if the caller is not the foreground task.
 */
bruce_result_t input__peek(bruce_input_event_t *out_event);

/* input__wait blocks until a press event arrives.
 *
 * Returns BRUCE_OK with the event code in *out_code, BRUCE_ERR_TIMEOUT if no
 * press event arrived within the timeout, or BRUCE_ERR_NOT_FOREGROUND if the
 * caller is not the foreground task.  Release and change events are ignored.
 */
bruce_result_t input__wait(uint32_t timeout_ms, int32_t *out_code);

/* input__check tests whether a press event for `code` is currently queued.
 *
 * If `consume` is true the first matching press event is removed from the queue;
 * if false the event is left in place.  Returns true if a matching press event
 * exists, false otherwise.  Always returns false for non-foreground tasks.
 *
 * This is the Core equivalent of the legacy `check(PrevPress)` / `check(SelPress)`
 * pattern used by the old Arduino loop code and JS bindings.
 */
bool input__check(int32_t code, bool consume);

/* input__inject pushes a normalized event into the input queue.  It is used
 * by input adapters (Bluetooth, GPIO, I2C, serial, ...) to feed the same
 * event pipeline as physical buttons and keyboard.
 *
 * Returns BRUCE_OK, BRUCE_ERR_PERMISSION if the caller lacks the `input`
 * permission, BRUCE_ERR_INVALID_ARGUMENT for a malformed event, or another
 * BRUCE_ERR_* value for internal failures.
 *
 * `event->timestamp_ms` and `event->source_task_id` are ignored by the core;
 * the core fills them in from the current time/task before queuing.
 */
bruce_result_t input__inject(const bruce_input_event_t *event);
