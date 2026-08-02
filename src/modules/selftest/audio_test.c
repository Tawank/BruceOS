#include "audio_test.h"

#include <stdio.h>

#include "core_sdk/audio.h"

bool selftest__run_audio_validation_case(void) {
    bool ok = audio__tone(0, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(BRUCE_AUDIO_MIN_FREQUENCY_HZ - 1, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(BRUCE_AUDIO_MAX_FREQUENCY_HZ + 1, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(440, 0, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(440, BRUCE_AUDIO_MAX_TONE_DURATION_MS + 1, false) == BRUCE_ERR_INVALID_ARGUMENT;
    printf("[selftest] audio/validation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
