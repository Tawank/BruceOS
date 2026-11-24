#pragma once

#include <stdbool.h>

#include "core_sdk/result.h"

/**
 * @brief Vibration motor.
 *
 * Plays short effects on the board's vibration motor, if it has one. The
 * motor is powered for an effect and turned off again shortly after the
 * last one, so apps just play effects when they want them.
 */

typedef enum {
    BRUCE_HAPTIC_CLICK = 0,
    BRUCE_HAPTIC_DOUBLE_CLICK,
    BRUCE_HAPTIC_BUZZ,
    BRUCE_HAPTIC_ALERT, /**< About 3/4 s, for something that wants attention (a ringing call). */
} bruce_haptic_effect_t;

/** @brief Returns whether the board has a vibration motor. */
bool haptic__is_available(void);

/**
 * @brief Starts an effect and returns at once; a new effect replaces one still playing.
 *
 * @param effect What to play.
 * @return BRUCE_ERR_UNSUPPORTED without a motor.
 * @permission none
 */
bruce_result_t haptic__play(bruce_haptic_effect_t effect);
