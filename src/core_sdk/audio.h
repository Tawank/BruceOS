#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_AUDIO_MIN_FREQUENCY_HZ 20u
#define BRUCE_AUDIO_MAX_FREQUENCY_HZ 20000u
#define BRUCE_AUDIO_MAX_TONE_DURATION_MS 60000u

/* Plays a square-wave tone using the board speaker or buzzer. Audio is not
 * permission-gated. A non-blocking tone is queued in a worker task; a blocking
 * tone returns after its duration. Concurrent tones are serialized (and, per
 * audio__stream_open() below, serialized against any open stream too) and the
 * bounded asynchronous queue returns BRUCE_ERR_RESOURCE_LIMIT when full. */
bruce_result_t audio__tone(uint32_t frequency_hz, uint32_t duration_ms, bool non_blocking);

/* Streaming PCM output, for apps synthesizing continuous audio themselves
 * (e.g. an emulated sound chip) instead of a single square-wave tone. Audio
 * is not permission-gated.
 *
 * Shares the same underlying output as audio__tone() above -- while a stream
 * is open, a concurrent audio__tone() call still works but blocks until the
 * stream write in progress, if any, finishes. Only one stream may be open
 * device-wide at a time; audio__stream_open() while another process's stream
 * is open returns BRUCE_ERR_BUSY. If the owning process exits or is killed
 * without calling audio__stream_close() itself, the stream is torn down
 * automatically.
 *
 * audio__stream_sample_rate() returns the fixed rate (Hz) all streams run
 * at; there is no way to open a stream at a different rate. On a backend
 * with no PCM support the returned rate is a nominal placeholder and
 * audio__stream_write() is a silent no-op, so callers do not need to special
 * case it.
 *
 * audio__stream_write() takes signed 16-bit PCM interleaved per the
 * `channels` passed to audio__stream_open() (1 = mono, 2 = stereo), always
 * at audio__stream_sample_rate() Hz, and blocks until the backend has
 * accepted the whole buffer -- that backpressure is what paces the caller to
 * real playback speed, the same way writing to a real sound card would. */
uint32_t audio__stream_sample_rate(void);
bruce_result_t audio__stream_open(uint8_t channels);
bruce_result_t audio__stream_write(const int16_t *samples, size_t frame_count);
bruce_result_t audio__stream_close(void);

/* Same contract as audio__stream_write() (signed 16-bit PCM, `channels`
 * interleaved, audio__stream_sample_rate() Hz), except the wait for the
 * backend's real-time backpressure happens on a dedicated background task
 * instead of the calling task. audio__stream_write() ties up its caller for
 * as long as the hardware takes to physically drain the samples -- fine for
 * a caller with nothing else to do, but a caller that alternates between
 * synthesizing audio and other real-time work (an emulator's CPU/PPU core,
 * a game loop) pays that wait serially on top of its own work every single
 * call. audio__stream_write_async() instead copies into a small internal
 * ring and returns as soon as the ring has room, so the caller can go on to
 * its next unit of work (e.g. the next emulated frame) while the actual
 * hardware write happens concurrently on the other core.
 *
 * This still blocks -- same backpressure guarantee as audio__stream_write()
 * -- but only once the caller has produced enough audio to fill the ring
 * without the background writer draining it in time, i.e. only once the
 * caller is genuinely running ahead of real time by more than the ring's
 * capacity affords. Under normal conditions (production and consumption
 * roughly keeping pace with each other) this returns quickly.
 *
 * Samples enqueued but not yet written to hardware are discarded, not
 * played, if the stream is closed (or its owning process exits/is killed)
 * before the background writer reaches them -- closing a stream always ends
 * in real silence on the output, never stale queued audio, at the cost of
 * dropping at most the ring's-worth of already-buffered-but-unplayed tail.
 * On a backend with no PCM support this is a no-op, like
 * audio__stream_write(). */
bruce_result_t audio__stream_write_async(const int16_t *samples, size_t frame_count);
