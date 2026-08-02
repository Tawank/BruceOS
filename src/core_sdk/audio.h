#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_AUDIO_MIN_FREQUENCY_HZ 20u
#define BRUCE_AUDIO_MAX_FREQUENCY_HZ 20000u
#define BRUCE_AUDIO_MAX_TONE_DURATION_MS 60000u

/* Plays a square-wave tone using the board speaker or buzzer. Audio is not
 * permission-gated. A non-blocking tone is queued in a worker task; a blocking
 * tone returns after its duration. Concurrent tones are serialized and the
 * bounded asynchronous queue returns BRUCE_ERR_RESOURCE_LIMIT when full. */
bruce_result_t audio__tone(uint32_t frequency_hz, uint32_t duration_ms, bool non_blocking);
