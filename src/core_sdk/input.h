#pragma once

#include <stdint.h>

#include "core_sdk/process.h"
#include "core_sdk/result.h"

/**
 * @brief Keyboard, button, touch, and encoder event queue.
 */

#ifdef __cplusplus
extern "C" {
#endif

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
 * character (0x00-0x7F); Ctrl+letter uses its standard ASCII control code
 * (Ctrl+A = 0x01 through Ctrl+Z = 0x1A). Navigation and action codes are defined below so that
 * physical buttons and Fn-chord keyboard bindings can emit the same
 * semantic events.
 */
#define BRUCE_INPUT_CODE_UP 0x03B
#define BRUCE_INPUT_CODE_DOWN 0x02E
#define BRUCE_INPUT_CODE_LEFT 0x02C
#define BRUCE_INPUT_CODE_RIGHT 0x02F
#define BRUCE_INPUT_CODE_SELECT 0x00A
#define BRUCE_INPUT_CODE_BACK 0x060
#define BRUCE_INPUT_CODE_MENU 0x106
#define BRUCE_INPUT_CODE_HOME 0x107
#define BRUCE_INPUT_CODE_DELETE 0x108
#define BRUCE_INPUT_CODE_PREV 0x109
#define BRUCE_INPUT_CODE_NEXT 0x10A
#define BRUCE_INPUT_CODE_ZOOM_OUT 0x10B
#define BRUCE_INPUT_CODE_ZOOM_IN 0x10C
#define BRUCE_INPUT_CODE_BUTTON_A 0x200
#define BRUCE_INPUT_CODE_BUTTON_B 0x201
#define BRUCE_INPUT_CODE_BUTTON_C 0x202
#define BRUCE_INPUT_CODE_BUTTON_X 0x203
#define BRUCE_INPUT_CODE_BUTTON_Y 0x204
#define BRUCE_INPUT_CODE_BUTTON_L1 0x205
#define BRUCE_INPUT_CODE_BUTTON_R1 0x206
#define BRUCE_INPUT_CODE_BUTTON_L2 0x207
#define BRUCE_INPUT_CODE_BUTTON_R2 0x208
#define BRUCE_INPUT_CODE_BUTTON_START 0x209
#define BRUCE_INPUT_CODE_BUTTON_SELECT 0x20A
#define BRUCE_INPUT_CODE_BUTTON_THUMB_L 0x20B
#define BRUCE_INPUT_CODE_BUTTON_THUMB_R 0x20C
#define BRUCE_INPUT_CODE_GAMEPAD_AXIS_X 0x300
#define BRUCE_INPUT_CODE_GAMEPAD_AXIS_Y 0x301
#define BRUCE_INPUT_CODE_GAMEPAD_AXIS_RX 0x302
#define BRUCE_INPUT_CODE_GAMEPAD_AXIS_RY 0x303

/*
 * For BRUCE_INPUT_TOUCH events, `code` is the tap's X coordinate and `value`
 * is its Y coordinate (panel pixel space, after any swap/mirror configured
 * for the board - see BRUCE_TOUCH_SWAP_XY/MIRROR_X/MIRROR_Y in Kconfig).
 * BRUCE_INPUT_PRESS/RELEASE mark the start/end of a touch; BRUCE_INPUT_CHANGE
 * is emitted while a touch moves without lifting (a drag).
 */
typedef struct {
    bruce_input_type_t type;
    bruce_input_action_t action;
    int32_t code;
    int32_t value;
    uint64_t timestamp_ms;
    bruce_process_id_t source_process_id;
} bruce_input_event_t;

/**
 * @brief Returns the next input event for the foreground app.
 *
 * `timeout_ms` controls blocking:
 *   - 0      : non-blocking; returns BRUCE_ERR_TIMEOUT immediately if no event
 *             is queued.
 *   - >0     : blocks up to the requested total elapsed milliseconds.
 *   - 0xFFFFFFFF (portMAX_DELAY equivalent) : block until an event arrives.
 *
 * Returns BRUCE_OK with *out_event filled, BRUCE_ERR_TIMEOUT if no event
 * arrived within the timeout, or BRUCE_ERR_NOT_FOREGROUND if the caller is not
 * the current foreground process. A blocked read is revoked immediately when its
 * foreground tenure ends; regaining foreground requires a new call. An
 * indefinite read is also interrupted by input deinitialization.
 *
 * Physical input is delivered only to the foreground process; background
 * processes must call process__foreground() or use input__inject() (which
 * requires the `input` permission).
 *
 * @param out_event Receives the popped event.
 * @param timeout_ms 0 for non-blocking, >0 to block up to that many milliseconds, or 0xFFFFFFFF to block indefinitely.
 */
bruce_result_t input__read(bruce_input_event_t *out_event, uint32_t timeout_ms);

/**
 * @brief Returns the next input event without waiting.
 *
 * It returns BRUCE_OK with *out_event filled, or BRUCE_ERR_TIMEOUT if no
 * event is available. It is equivalent to input__read(out_event, 0).
 *
 * Typical polling loop:
 *   bruce_input_event_t ev;
 *   while (input__poll(&ev) == BRUCE_OK) { ... handle ev ... }
 *
 * @param out_event Receives the popped event.
 */
static inline bruce_result_t input__poll(bruce_input_event_t *out_event) { return input__read(out_event, 0); }

/**
 * @brief Removes all queued input events.
 *
 * It is useful when switching screens so that stale presses do not affect
 * the new UI. Returns BRUCE_OK or BRUCE_ERR_NOT_FOREGROUND if the caller is
 * not the foreground process.
 */
bruce_result_t input__flush(void);

/**
 * @brief Returns the next queued event without removing it.
 *
 * Returns BRUCE_OK with *out_event filled, BRUCE_ERR_TIMEOUT if the queue
 * is empty, or BRUCE_ERR_NOT_FOREGROUND if the caller is not the foreground
 * process.
 *
 * @param out_event Receives the next queued event.
 */
bruce_result_t input__peek(bruce_input_event_t *out_event);

/**
 * @brief Waits for a press event.
 *
 * Returns BRUCE_OK with the event code in *out_code, BRUCE_ERR_TIMEOUT if
 * no press event arrived within the timeout, or BRUCE_ERR_NOT_FOREGROUND if
 * the caller is not the foreground process. Release and change events are
 * ignored.
 *
 * @param timeout_ms Maximum time to wait, in milliseconds.
 * @param out_code Receives the pressed event's code.
 */
bruce_result_t input__wait(uint32_t timeout_ms, int32_t *out_code);

/**
 * @brief Checks whether a press event for `code` is queued.
 *
 * If `consume` is true the first matching press event is removed from the
 * queue; if false the event is left in place. Returns true if a matching
 * press event exists, false otherwise. Always returns false for
 * non-foreground processes.
 *
 * This is the Core equivalent of the legacy `check(PrevPress)` /
 * `check(SelPress)` pattern used by the old Arduino loop code and JS
 * bindings.
 *
 * @param code Event code to test for, e.g. BRUCE_INPUT_CODE_SELECT.
 * @param consume If true, removes the matching event from the queue.
 */
bool input__check(int32_t code, bool consume);

/**
 * @brief Adds an event to the input queue.
 *
 * It is used by input adapters (Bluetooth, GPIO, I2C, serial, ...) to feed
 * the same event pipeline as physical buttons and keyboard.
 *
 * Returns BRUCE_OK, BRUCE_ERR_PERMISSION if the caller lacks the `input`
 * permission, BRUCE_ERR_INVALID_ARGUMENT for a malformed event, or another
 * BRUCE_ERR_* value for internal failures.
 *
 * `event->timestamp_ms` and `event->source_process_id` are ignored by the
 * core; the core fills them in from the current time/process before
 * queuing.
 *
 * @param event Event to inject.
 * @permission input
 */
bruce_result_t input__inject(const bruce_input_event_t *event);

#ifdef __cplusplus
}
#endif
