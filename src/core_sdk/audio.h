#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Tone and PCM audio playback.
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
 * @brief Opens PCM audio output.
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
 * @brief Returns how many PCM frames can be written immediately.
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

/**
 * @brief Starts playing a local audio file in the background and returns
 * immediately.
 *
 * WAV (8- or 16-bit PCM, mono or stereo, any sample rate -- resampled to
 * audio__stream_sample_rate()) and MP3 are understood; anything else returns
 * BRUCE_ERR_UNSUPPORTED without side effects, checked synchronously before
 * this returns (so a bad path or unsupported file is reported right away,
 * not discovered later in the background). A valid file is then handed off
 * to a dedicated background process this call spawns, built on top of
 * audio__stream_open()/write()/close(), so playback shares their one-
 * stream-at-a-time exclusivity (that background process gets BRUCE_ERR_BUSY
 * internally, silently, if a tone or another file is already playing --
 * call audio__file_is_playing() first if the caller needs to know).
 * audio__file_pause()/_resume()/_seek()/_stop() below control whatever file
 * is currently playing from any process, including this call's own caller
 * -- they just set a request flag the background process's loop reacts to.
 * audio__file_is_open()/_is_playing()/_position_ms()/_duration_ms() are
 * likewise safe to call from anywhere. The file closes automatically if
 * its background process is killed without audio__file_stop() being called
 * first. No permission is required beyond already being able to read
 * `path` -- any process can start playback.
 *
 * MP3 playback decodes via esp_audio_simple_player, whose decoder runs on
 * its own internal task rather than this process's; since a storage file
 * handle can't be read across tasks that way, compressed bytes are streamed
 * through a small fixed-size RAM ring instead (refilled from storage by
 * this process's own task), so supported file size isn't bounded by RAM --
 * unlike, say, loading the whole file into RAM up front would be, which
 * matters on the boards this targets that have no PSRAM at all. seek()
 * returns BRUCE_ERR_UNSUPPORTED and position_ms()/duration_ms() return 0
 * for MP3 (position/duration tracking isn't implemented for it).
 *
 * This call itself never waits for the file to actually open -- that still
 * happens in the background process, same as decoding. audio__file_is_open()
 * covers the gap: it reports true from the moment this function returns
 * until that process either opens the file for real or fails, so a caller
 * that polls is_open()/is_playing() right away (to wait for playback to
 * finish, say) can't mistake "hasn't opened yet" for "already finished".
 *
 * @param path Absolute, normalized path to the audio file.
 * @permission none
 */
bruce_result_t audio__file_play(const char *path);

/**
 * @brief Pauses the file currently playing via audio__file_play(), without
 * closing it.
 *
 * @permission none
 */
bruce_result_t audio__file_pause(void);

/**
 * @brief Resumes a file paused with audio__file_pause().
 *
 * @permission none
 */
bruce_result_t audio__file_resume(void);

/**
 * @brief Requests that the currently playing file jump to an absolute
 * position.
 *
 * Takes effect the next time audio__file_play()'s own loop polls for it --
 * not necessarily instantaneous. A position past the end of the file clamps
 * to the end (audio__file_play() then finishes as if playback reached it
 * normally). Returns BRUCE_ERR_UNSUPPORTED for a currently-playing MP3 (see
 * audio__file_play()).
 *
 * @param position_ms Target position, in milliseconds from the start.
 * @permission none
 */
bruce_result_t audio__file_seek(uint32_t position_ms);

/**
 * @brief Stops the file currently playing via audio__file_play() and closes
 * it.
 *
 * Takes effect the next time audio__file_play()'s own loop polls for it,
 * same as audio__file_seek().
 *
 * @permission none
 */
bruce_result_t audio__file_stop(void);

/**
 * @brief True while a file started by audio__file_play() is open (playing
 * or paused), or still in the process of opening.
 *
 * True from the moment audio__file_play() returns, since opening happens in
 * the background; false once it either fails to open, or finishes/is
 * stopped after having opened.
 *
 * @permission none
 */
bool audio__file_is_open(void);

/**
 * @brief True while a file opened by audio__file_play() is actively
 * playing (false if paused, stopped, finished, or never opened).
 *
 * @permission none
 */
bool audio__file_is_playing(void);

/**
 * @brief Current peak level of the file playing via audio__file_play(), as
 * a percentage of full scale (0-100).
 *
 * Reflects whichever chunk of decoded PCM was most recently handed to the
 * output stream, so it updates several times a second during playback;
 * 0 while paused, stopped, or no file is open. A plain sample peak, not a
 * calibrated loudness measurement -- meant for a simple level meter or a
 * music-reactive animation, not accurate metering.
 *
 * @permission none
 */
uint8_t audio__file_level(void);

/**
 * @brief Current playback position of the file opened by
 * audio__file_play(), in milliseconds from the start.
 *
 * 0 if no file is open.
 *
 * @permission none
 */
uint32_t audio__file_position_ms(void);

/**
 * @brief Total duration of the file opened by audio__file_play(), in
 * milliseconds.
 *
 * 0 if no file is open or its length couldn't be determined.
 *
 * @permission none
 */
uint32_t audio__file_duration_ms(void);
