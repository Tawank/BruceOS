#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Tone and streaming PCM audio output.
 */

#define BRUCE_AUDIO_MIN_FREQUENCY_HZ 20u
#define BRUCE_AUDIO_MAX_FREQUENCY_HZ 20000u
#define BRUCE_AUDIO_MAX_TONE_DURATION_MS 60000u

/**
 * @brief Plays a square-wave tone using the board speaker or buzzer.
 *
 * A non-blocking tone is queued in a worker task; a blocking tone returns
 * after its duration. Concurrent tones are serialized (and, per
 * audio__stream_open() below, serialized against any open stream too) and
 * the bounded asynchronous queue returns BRUCE_ERR_RESOURCE_LIMIT when full.
 *
 * @param frequency_hz Tone frequency in Hz (BRUCE_AUDIO_MIN/MAX_FREQUENCY_HZ).
 * @param duration_ms Tone duration in milliseconds (up to BRUCE_AUDIO_MAX_TONE_DURATION_MS).
 * @param non_blocking If true, queue the tone and return immediately instead of waiting out its duration.
 * @permission none
 */
bruce_result_t audio__tone(uint32_t frequency_hz, uint32_t duration_ms, bool non_blocking);

/**
 * @brief Fixed sample rate (Hz) that every stream runs at.
 *
 * There is no way to open a stream at a different rate. On a backend with
 * no PCM support the returned rate is a nominal placeholder.
 *
 * @permission none
 */
uint32_t audio__stream_sample_rate(void);

/**
 * @brief Opens the device-wide streaming PCM output, for apps synthesizing
 * continuous audio themselves (e.g. an emulated sound chip) instead of a
 * single square-wave tone.
 *
 * Shares the same underlying output as audio__tone() -- while a stream is
 * open, a concurrent audio__tone() call still works but blocks until the
 * stream write in progress, if any, finishes. Only one stream may be open
 * device-wide at a time; calling this while another process's stream is
 * open returns BRUCE_ERR_BUSY. If the owning process exits or is killed
 * without calling audio__stream_close() itself, the stream is torn down
 * automatically.
 *
 * @param channels Channel count: 1 = mono, 2 = stereo.
 * @permission none
 */
bruce_result_t audio__stream_open(uint8_t channels);

/**
 * @brief Number of PCM frames that can currently be written without blocking/dropping.
 *
 * Apps that cannot drop audio can consult this before synthesizing more.
 *
 * @permission none
 */
size_t audio__stream_writable_frames(void);

/**
 * @brief Writes PCM frames to the open stream.
 *
 * Takes signed 16-bit PCM interleaved per the `channels` passed to
 * audio__stream_open(), always at audio__stream_sample_rate() Hz. It never
 * blocks: it returns the number of frames actually copied to the bounded
 * stream FIFO. On a backend with no PCM support this is a silent no-op.
 *
 * @param samples Interleaved signed 16-bit PCM samples to write.
 * @param frame_count Number of frames available in samples.
 * @permission none
 */
size_t audio__stream_write(const int16_t *samples, size_t frame_count);

/**
 * @brief Closes the streaming PCM output opened by audio__stream_open().
 *
 * @permission none
 */
bruce_result_t audio__stream_close(void);
