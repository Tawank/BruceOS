#include "audio_test.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "core/process/process.h"
#include "core_sdk/audio.h"
#include "core_sdk/process.h"

bool selftest__run_audio_validation_case(void) {
    bool ok = audio__tone(0, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(BRUCE_AUDIO_MIN_FREQUENCY_HZ - 1, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(BRUCE_AUDIO_MAX_FREQUENCY_HZ + 1, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(440, 0, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(440, BRUCE_AUDIO_MAX_TONE_DURATION_MS + 1, false) == BRUCE_ERR_INVALID_ARGUMENT;
    printf("[selftest] audio/validation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_audio_kill_mid_tone_case                                    */
/* ------------------------------------------------------------------------ */

static int selftest__audio_kill_mid_tone_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    (void)audio__tone(880, 300, false);
    return 0;
}

/* Regression guard: audio__play() must wrap its s_audio_mutex/peripheral
 * section in process_registry__operation_begin()/_end() so process__kill()
 * waits for it to finish tearing down instead of deleting the task mid-tone.
 * Without that, the killed task would leak the speaker enabled (stuck
 * playing) and leave s_audio_mutex held forever, hanging every later
 * audio__tone() call - including the follow-up call below, which would never
 * return instead of this case failing cleanly. */
bool selftest__run_audio_kill_mid_tone_case(void) {
    process_create_params_t params = {
        .name = "selftest_audio_kill_mid_tone",
        .entry = selftest__audio_kill_mid_tone_entry,
        .argc = 0,
        .argv = NULL,
        .built_in = true,
        .gui_requested = false,
        .permission_key = "",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    bool created = process_registry__create(&params, &id) == BRUCE_OK;
    vTaskDelay(pdMS_TO_TICKS(50));
    bruce_result_t killed = created ? process__kill(id) : BRUCE_ERR_INTERNAL;
    bruce_result_t follow_up = audio__tone(880, 10, false);

    bool ok = created && killed == BRUCE_OK && follow_up == BRUCE_OK;
    printf(
        "[selftest] audio/kill-mid-tone: %s (killed=%d follow_up=%d)\n", ok ? "OK" : "FAIL", killed, follow_up
    );
    return ok;
}
