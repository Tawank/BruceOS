#include "audio_test.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "core/process/process.h"
#include "core_sdk/audio.h"
#include "core_sdk/process.h"
#include "core_sdk/storage.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

bool selftest__run_audio_validation_case(void) {
    bool ok = audio__tone(0, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(BRUCE_AUDIO_MIN_FREQUENCY_HZ - 1, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(BRUCE_AUDIO_MAX_FREQUENCY_HZ + 1, 100, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(440, 0, false) == BRUCE_ERR_INVALID_ARGUMENT &&
              audio__tone(440, BRUCE_AUDIO_MAX_TONE_DURATION_MS + 1, false) == BRUCE_ERR_INVALID_ARGUMENT;
    printf("[selftest] audio/validation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_audio_stream_nonblocking_case(void) {
    int16_t samples[32] = {0};
    bool opened = audio__stream_open(1) == BRUCE_OK;
    size_t writable = audio__stream_writable_frames();
    size_t accepted = opened ? audio__stream_write(samples, 32) : 0;
    bruce_result_t closed = opened ? audio__stream_close() : BRUCE_ERR_INTERNAL;
    bool ok = opened && writable >= 32 && accepted == 32 && closed == BRUCE_OK;
    printf("[selftest] audio/stream-nonblocking: %s\n", ok ? "OK" : "FAIL");
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

/* ------------------------------------------------------------------------ */
/* Mixer                                                                     */
/* ------------------------------------------------------------------------ */

#define AUDIO_TEST__SLOTS 4 /* AUDIO__STREAM_SLOTS in core/audio/audio.c */
#define AUDIO_TEST__RING_FRAMES 4096u

/* Helper children each open a stream, report it through these flags, and hold
 * it until the parent clears s_hold (or kills them, in the kill case). */
static atomic_int s_children_open;
static atomic_int s_children_failed;
static atomic_bool s_hold;
static atomic_bool s_child_leave_open;

static int audio_test__holder_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int16_t samples[64] = {0};
    if (audio__stream_open(1) != BRUCE_OK || audio__stream_write(samples, 64) != 64) {
        atomic_fetch_add(&s_children_failed, 1);
        return 1;
    }
    atomic_fetch_add(&s_children_open, 1);
    while (atomic_load(&s_hold)) vTaskDelay(pdMS_TO_TICKS(10));
    if (!atomic_load(&s_child_leave_open)) (void)audio__stream_close();
    return 0;
}

static bool audio_test__spawn_holder(bruce_process_id_t *id) {
    process_create_params_t params = {
        .name = "selftest_audio_holder",
        .entry = audio_test__holder_entry,
        .built_in = true,
        .permission_key = "",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    return process_registry__create(&params, id) == BRUCE_OK;
}

static bool audio_test__wait_children_open(int expected) {
    for (int i = 0; i < 100 && atomic_load(&s_children_open) < expected; ++i) {
        if (atomic_load(&s_children_failed) != 0) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return atomic_load(&s_children_open) == expected;
}

static void audio_test__reset_children(void) {
    atomic_store(&s_children_open, 0);
    atomic_store(&s_children_failed, 0);
    atomic_store(&s_child_leave_open, false);
    atomic_store(&s_hold, true);
}

/* Several apps can hold streams at once, the slot table limit is enforced
 * with BUSY, a process can't open two, and the slots are reusable afterward.
 * The parent counts as one of the AUDIO_TEST__SLOTS. */
bool selftest__run_audio_mixer_concurrent_case(void) {
    int16_t samples[64] = {0};
    bruce_process_id_t ids[AUDIO_TEST__SLOTS] = {0};
    audio_test__reset_children();

    bool opened = audio__stream_open(2) == BRUCE_OK;
    bool double_open_busy = audio__stream_open(1) == BRUCE_ERR_BUSY;
    int spawned = 0;
    for (; opened && spawned < AUDIO_TEST__SLOTS - 1; ++spawned) {
        if (!audio_test__spawn_holder(&ids[spawned])) break;
    }
    bool children_open = spawned == AUDIO_TEST__SLOTS - 1 && audio_test__wait_children_open(spawned);
    bool parent_writes = opened && audio__stream_write(samples, 32) == 32;

    /* All slots taken: one more stream, from another process, must be refused. */
    bruce_process_id_t extra = BRUCE_PROCESS_ID_INVALID;
    bool extra_refused = false;
    if (children_open && audio_test__spawn_holder(&extra)) {
        vTaskDelay(pdMS_TO_TICKS(100));
        extra_refused = atomic_load(&s_children_failed) == 1 && atomic_load(&s_children_open) == spawned;
    }

    atomic_store(&s_hold, false);
    for (int i = 0; i < spawned; ++i) (void)process__wait_status(ids[i], 1000, &(bruce_process_status_t){0});
    if (extra != BRUCE_PROCESS_ID_INVALID) {
        (void)process__wait_status(extra, 1000, &(bruce_process_status_t){0});
    }
    bool still_writes = opened && audio__stream_write(samples, 32) == 32;
    bool closed = opened && audio__stream_close() == BRUCE_OK;
    bool reopened = audio__stream_open(1) == BRUCE_OK && audio__stream_close() == BRUCE_OK;

    bool ok = opened && double_open_busy && children_open && parent_writes && extra_refused && still_writes &&
              closed && reopened;
    printf(
        "[selftest] audio/mixer-concurrent: %s (dbl=%d kids=%d w=%d full=%d w2=%d close=%d reopen=%d)\n",
        ok ? "OK" : "FAIL", double_open_busy, children_open, parent_writes, extra_refused, still_writes, closed,
        reopened
    );
    return ok;
}

/* A tone must neither fail nor stall while a stream is open, and the stream
 * must keep accepting data and drain it afterward. */
bool selftest__run_audio_mixer_tone_over_stream_case(void) {
    int16_t *samples = malloc(2048 * sizeof(*samples));
    if (samples == NULL) return false;
    for (size_t i = 0; i < 2048; ++i) samples[i] = (i & 32) ? 2000 : -2000;

    bool opened = audio__stream_open(1) == BRUCE_OK;
    size_t accepted = opened ? audio__stream_write(samples, 2048) : 0;
    bruce_result_t tone = audio__tone(880, 60, false);
    bruce_result_t async_tone = audio__tone(660, 30, true);
    vTaskDelay(pdMS_TO_TICKS(150));
    /* The mixer has consumed what was queued: the whole ring is writable again. */
    bool drained = opened && audio__stream_writable_frames() >= AUDIO_TEST__RING_FRAMES;
    bool more = opened && audio__stream_write(samples, 2048) == 2048;
    bool closed = opened && audio__stream_close() == BRUCE_OK;
    free(samples);

    bool ok = opened && accepted == 2048 && tone == BRUCE_OK && async_tone == BRUCE_OK && drained && more && closed;
    printf(
        "[selftest] audio/mixer-tone-over-stream: %s (acc=%u tone=%d async=%d drained=%d more=%d)\n",
        ok ? "OK" : "FAIL", (unsigned)accepted, tone, async_tone, drained, more
    );
    return ok;
}

/* A process killed while holding a stream must give its slot back. */
bool selftest__run_audio_mixer_kill_frees_slot_case(void) {
    bruce_process_id_t ids[AUDIO_TEST__SLOTS] = {0};
    audio_test__reset_children();
    atomic_store(&s_child_leave_open, true); /* exit/kill without audio__stream_close() */

    int spawned = 0;
    for (; spawned < AUDIO_TEST__SLOTS; ++spawned) {
        if (!audio_test__spawn_holder(&ids[spawned])) break;
    }
    bool all_open = spawned == AUDIO_TEST__SLOTS && audio_test__wait_children_open(spawned);
    bool full_refused = all_open && audio__stream_open(1) == BRUCE_ERR_BUSY;
    bool killed = true;
    for (int i = 0; i < spawned; ++i) killed = process__kill(ids[i]) == BRUCE_OK && killed;
    vTaskDelay(pdMS_TO_TICKS(50));
    bool reopened = audio__stream_open(1) == BRUCE_OK;
    if (reopened) (void)audio__stream_close();
    atomic_store(&s_hold, false);

    bool ok = all_open && full_refused && killed && reopened;
    printf(
        "[selftest] audio/mixer-kill-frees-slot: %s (open=%d full=%d killed=%d reopen=%d)\n",
        ok ? "OK" : "FAIL", all_open, full_refused, killed, reopened
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* File playback leaks                                                       */
/* ------------------------------------------------------------------------ */

#define AUDIO_TEST__LEAK_ITERATIONS 5
/* Allocator bookkeeping and task-list churn can shift the numbers by a little;
 * a real per-play leak (a ring, a decoder, a file handle) is orders larger. */
#define AUDIO_TEST__LEAK_TOLERANCE_BYTES 512

static bool audio_test__write_file(const char *path, const void *data, size_t size) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
                      &file) != BRUCE_OK) {
        return false;
    }
    size_t written = 0;
    bool ok = storage__write(file, data, size, &written) == BRUCE_OK && written == size;
    (void)storage__close(file);
    return ok;
}

static void audio_test__put_u32(uint8_t *out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

/* Mono 16-bit 22.05 kHz square wave, ~0.5 s: exercises the resampler. */
static bool audio_test__write_wav(const char *path) {
    enum { RATE = 22050, SAMPLES = 11025 };
    const size_t wav_size = 44 + SAMPLES * 2;
    uint8_t *wav = calloc(1, wav_size);
    if (wav == NULL) return false;
    memcpy(wav, "RIFF", 4);
    audio_test__put_u32(wav + 4, (uint32_t)wav_size - 8);
    memcpy(wav + 8, "WAVEfmt ", 8);
    audio_test__put_u32(wav + 16, 16);
    wav[20] = 1; /* PCM */
    wav[22] = 1; /* mono */
    audio_test__put_u32(wav + 24, RATE);
    audio_test__put_u32(wav + 28, RATE * 2);
    wav[32] = 2;
    wav[34] = 16;
    memcpy(wav + 36, "data", 4);
    audio_test__put_u32(wav + 40, SAMPLES * 2);
    for (int i = 0; i < SAMPLES; ++i) {
        int16_t sample = (i / 25) & 1 ? 3000 : -3000;
        wav[44 + i * 2] = (uint8_t)sample;
        wav[45 + i * 2] = (uint8_t)((uint16_t)sample >> 8);
    }
    bool ok = audio_test__write_file(path, wav, wav_size);
    free(wav);
    return ok;
}

/* MPEG-1 Layer III, 128 kbps, 44.1 kHz frames with an all-zero body: each is
 * a valid frame that decodes to silence, so no encoder or embedded asset is
 * needed. 417 bytes per frame (no padding), ~26 ms each. */
static bool audio_test__write_mp3(const char *path) {
    enum { FRAME_BYTES = 417, FRAMES = 40 };
    static uint8_t frame[FRAME_BYTES];
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
                      &file) != BRUCE_OK) {
        return false;
    }
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;
    bool ok = true;
    for (int i = 0; i < FRAMES && ok; ++i) {
        size_t written = 0;
        ok = storage__write(file, frame, sizeof(frame), &written) == BRUCE_OK && written == sizeof(frame);
    }
    (void)storage__close(file);
    return ok;
}

static bool audio_test__wait_file_closed(uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; waited += 20) {
        if (!audio__file_is_open()) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !audio__file_is_open();
}

/* One play: start, let it run briefly, stop (or let it finish on its own when
 * `to_end`), and wait for the background process to be fully gone. */
static bool audio_test__play_once(const char *path, bool to_end) {
    if (audio__file_play(path) != BRUCE_OK) return false;
    if (!to_end) {
        vTaskDelay(pdMS_TO_TICKS(80));
        (void)audio__file_stop();
    }
    if (!audio_test__wait_file_closed(4000)) return false;
    vTaskDelay(pdMS_TO_TICKS(200)); /* idle task reclaims the deleted tasks' memory */
    return true;
}

static bool audio_test__leak_run(const char *name, const char *path) {
    /* First pass pays for one-time lazy allocations (mutexes, I2S channel,
     * decoder registration); only later passes are measured. */
    bool ok = audio_test__play_once(path, false);
    size_t baseline_total = esp_get_free_heap_size();
    size_t baseline_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    for (int i = 0; ok && i < AUDIO_TEST__LEAK_ITERATIONS; ++i) {
        ok = audio_test__play_once(path, /*to_end=*/i == AUDIO_TEST__LEAK_ITERATIONS - 1);
    }
    long leaked_total = (long)baseline_total - (long)esp_get_free_heap_size();
    long leaked_internal = (long)baseline_internal - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool no_leak = leaked_total <= AUDIO_TEST__LEAK_TOLERANCE_BYTES &&
                   leaked_internal <= AUDIO_TEST__LEAK_TOLERANCE_BYTES;
    printf(
        "[selftest] audio/%s-leak: %s (plays_ok=%d total=%ld internal=%ld bytes)\n", name,
        ok && no_leak ? "OK" : "FAIL", ok, leaked_total, leaked_internal
    );
    return ok && no_leak;
}

bool selftest__run_audio_file_wav_leak_case(void) {
    static const char path[] = "/selftest_audio_leak.wav";
    bool ok = audio_test__write_wav(path) && audio_test__leak_run("wav", path);
    (void)storage__remove(path);
    return ok;
}

bool selftest__run_audio_file_mp3_leak_case(void) {
    static const char path[] = "/selftest_audio_leak.mp3";
    bool ok = audio_test__write_mp3(path) && audio_test__leak_run("mp3", path);
    (void)storage__remove(path);
    return ok;
}
