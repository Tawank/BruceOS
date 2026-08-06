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
