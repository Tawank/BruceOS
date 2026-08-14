#include "core_sdk/audio.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "core/config/config.h"
#include "core/process/process.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BRUCE_AUDIO_BACKEND_I2S
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#define AUDIO__I2S_SAMPLE_RATE 48000u
#define AUDIO__I2S_BUFFER_FRAMES 256u
#define AUDIO__I2S_BCLK_GPIO ((gpio_num_t)CONFIG_BRUCE_AUDIO_I2S_BCLK_GPIO)
#define AUDIO__I2S_DATA_GPIO ((gpio_num_t)CONFIG_BRUCE_AUDIO_I2S_DATA_GPIO)
#define AUDIO__I2S_LRCLK_GPIO ((gpio_num_t)CONFIG_BRUCE_AUDIO_I2S_LRCLK_GPIO)
#elif CONFIG_BRUCE_AUDIO_BACKEND_LEDC_BUZZER
#include "driver/ledc.h"
#define AUDIO__OUTPUT_GPIO CONFIG_BRUCE_AUDIO_BUZZER_GPIO
#define AUDIO__LEDC_MODE LEDC_LOW_SPEED_MODE
#define AUDIO__LEDC_TIMER LEDC_TIMER_1
#define AUDIO__LEDC_CHANNEL LEDC_CHANNEL_6
#define AUDIO__MAX_FREQUENCY_13_BIT_HZ 9765u
#endif

#define AUDIO__MAX_ASYNC_TONES 4u

typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint32_t volume;
} audio__tone_params_t;

static StaticSemaphore_t s_audio_mutex_storage;
static SemaphoreHandle_t s_audio_mutex;
static portMUX_TYPE s_audio_init_mux = portMUX_INITIALIZER_UNLOCKED;
static size_t s_async_tone_count;

/* Streaming-PCM state (audio__stream_open/write/close below), guarded by
 * s_audio_mutex like everything else in this file. Exactly one stream may be
 * open device-wide at a time, same exclusivity as tone playback -- both
 * ultimately drive the same physical bus. */
static volatile bool s_stream_open;
static bruce_process_id_t s_stream_owner;
static bruce_resource_id_t s_stream_resource;
static uint8_t s_stream_channels;

static void audio__ensure_mutex(void) {
    if (s_audio_mutex != NULL) return;
    portENTER_CRITICAL(&s_audio_init_mux);
    if (s_audio_mutex == NULL) { s_audio_mutex = xSemaphoreCreateMutexStatic(&s_audio_mutex_storage); }
    portEXIT_CRITICAL(&s_audio_init_mux);
}

#if CONFIG_BRUCE_AUDIO_BACKEND_I2S && !CONFIG_BRUCE_QEMU_TEST_MODE
static i2s_chan_handle_t s_i2s_tx_channel;
static bool s_i2s_tx_ready;

/* i2s_new_channel()'s own default (I2S_CHANNEL_DEFAULT_CONFIG) is 6
 * descriptors x 240 frames, but that default is IDF's to change; pinning it
 * explicitly below means AUDIO__I2S_DMA_RING_FRAMES is guaranteed to
 * actually match the hardware ring, not just whatever IDF happened to
 * default to when this was written -- audio__i2s_flush_silence_locked()
 * further down relies on that to fully silence the ring. */
#define AUDIO__I2S_DMA_DESC_COUNT 6u
#define AUDIO__I2S_DMA_FRAME_COUNT 240u
#define AUDIO__I2S_DMA_RING_FRAMES (AUDIO__I2S_DMA_DESC_COUNT * AUDIO__I2S_DMA_FRAME_COUNT)
#define AUDIO__STREAM_RING_FRAMES 4096u

/* A single producer app writes the FIFO while the I2S ISR consumes it. The
 * cursors are monotonically increasing, so each side owns one cursor and no
 * lock or scheduler handoff is needed on the real-time path. */
static int16_t *s_stream_ring;
static atomic_uint_fast32_t s_stream_write_cursor;
static atomic_uint_fast32_t s_stream_read_cursor;

static bool IRAM_ATTR audio__i2s_on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context) {
    (void)handle;
    (void)context;
    int16_t *output = event->dma_buf;
    size_t frames = event->size / (2u * sizeof(*output));
    if (!s_stream_open) return false;
    uint32_t read_cursor = atomic_load_explicit(&s_stream_read_cursor, memory_order_relaxed);
    uint32_t write_cursor = atomic_load_explicit(&s_stream_write_cursor, memory_order_acquire);
    uint32_t available = write_cursor - read_cursor;
    if (available > AUDIO__STREAM_RING_FRAMES) available = 0;
    if (available > frames) available = (uint32_t)frames;

    for (uint32_t i = 0; i < available; ++i) {
        uint32_t ring_index = (read_cursor + i) % AUDIO__STREAM_RING_FRAMES;
        output[i * 2u] = s_stream_ring[ring_index * 2u];
        output[i * 2u + 1u] = s_stream_ring[ring_index * 2u + 1u];
    }
    if (available < frames) memset(&output[available * 2u], 0, (frames - available) * 2u * sizeof(*output));
    atomic_store_explicit(&s_stream_read_cursor, read_cursor + available, memory_order_release);
    return false;
}

/* Lazily creates and enables the one persistent TX channel, reused by every
 * tone and every stream write instead of being torn down between them.
 *
 * This used to be a create-enable-...-disable-delete cycle run fresh on
 * every single tone. i2s_del_channel() releases the BCLK/WS/DOUT GPIOs back
 * to plain, floating inputs once nothing is being played -- and the board's
 * I2S amplifier chip apparently free-runs off whatever noise it picks up on
 * a floating clock/data line rather than actually muting, which is what was
 * behind the "speaker keeps hissing until the board is fully power-cycled"
 * report: nothing on the ESP32 side is stuck (the channel really is gone),
 * but a soft reset reinitializes those pins the exact same way at boot --
 * floating until the first tone -- so it never cleared the amp's noise
 * either. Keeping the channel allocated and enabled for good means those
 * pins are always actively driven by the I2S peripheral, either with real
 * audio or with the explicit silence written at the end of every call below,
 * so the amp always sees a valid, defined signal and never has a floating
 * input to react to. */
static bruce_result_t audio__i2s_ensure_channel_locked(void) {
    if (s_i2s_tx_ready) return BRUCE_OK;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO__I2S_DMA_DESC_COUNT;
    channel_config.dma_frame_num = AUDIO__I2S_DMA_FRAME_COUNT;
    channel_config.auto_clear_after_cb = false;
    if (i2s_new_channel(&channel_config, &s_i2s_tx_channel, NULL) != ESP_OK) return BRUCE_ERR_BUSY;

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO__I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO__I2S_BCLK_GPIO,
            .ws = AUDIO__I2S_LRCLK_GPIO,
            .dout = AUDIO__I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    const i2s_event_callbacks_t callbacks = {.on_sent = audio__i2s_on_sent};
    if (i2s_channel_init_std_mode(s_i2s_tx_channel, &config) != ESP_OK ||
        i2s_channel_register_event_callback(s_i2s_tx_channel, &callbacks, NULL) != ESP_OK ||
        i2s_channel_enable(s_i2s_tx_channel) != ESP_OK) {
        i2s_del_channel(s_i2s_tx_channel);
        s_i2s_tx_channel = NULL;
        return BRUCE_ERR_INTERNAL;
    }
    s_i2s_tx_ready = true;
    return BRUCE_OK;
}

/* Writes `frames` of interleaved stereo samples, blocking until the hardware
 * has accepted them (the same backpressure a real sound card gives you). */
static bruce_result_t audio__i2s_write_locked(const int16_t *interleaved_stereo, uint32_t frames) {
    size_t bytes_written = 0;
    size_t bytes = (size_t)frames * 2u * sizeof(int16_t);
    if (i2s_channel_write(s_i2s_tx_channel, interleaved_stereo, bytes, &bytes_written, portMAX_DELAY) != ESP_OK ||
        bytes_written != bytes) {
        return BRUCE_ERR_IO;
    }
    return BRUCE_OK;
}

/* Writes enough true-zero frames to cover the *entire* DMA ring
 * (AUDIO__I2S_DMA_RING_FRAMES), not just one AUDIO__I2S_BUFFER_FRAMES
 * chunk. i2s_channel_write() only overwrites as many ring descriptors as
 * the frames it's given -- a write smaller than the ring leaves whatever
 * *other* descriptors were still holding real, non-zero audio (e.g. because
 * a producer fell behind and the ring had genuinely filled with several
 * frames' worth of real samples) untouched, and the DMA keeps circulating
 * and replaying those forever once nothing writes anything newer. That's
 * exactly the "one sound repeats constantly until the board is
 * power-cycled" failure mode this exists to prevent: a flush has to reach
 * every descriptor, not just the next one in line. Writing a bit more than
 * the ring's total capacity guarantees that regardless of where the ring's
 * write pointer currently is. */
static bruce_result_t audio__i2s_flush_silence_locked(void) {
    int16_t silence[AUDIO__I2S_BUFFER_FRAMES * 2u];
    memset(silence, 0, sizeof(silence));
    bruce_result_t result = BRUCE_OK;
    for (uint32_t frames_written = 0; frames_written < AUDIO__I2S_DMA_RING_FRAMES && result == BRUCE_OK;
         frames_written += AUDIO__I2S_BUFFER_FRAMES) {
        result = audio__i2s_write_locked(silence, AUDIO__I2S_BUFFER_FRAMES);
    }
    return result;
}

static bruce_result_t audio__play_i2s(const audio__tone_params_t *params) {
    bruce_result_t result = audio__i2s_ensure_channel_locked();
    if (result != BRUCE_OK) return result;

    int16_t samples[AUDIO__I2S_BUFFER_FRAMES * 2u];
    int16_t amplitude = (int16_t)((INT16_MAX * params->volume) / 100u);
    uint32_t phase = 0;
    uint32_t frames_remaining = (AUDIO__I2S_SAMPLE_RATE * params->duration_ms) / 1000u;
    while (frames_remaining > 0) {
        uint32_t frames = frames_remaining < AUDIO__I2S_BUFFER_FRAMES ? frames_remaining : AUDIO__I2S_BUFFER_FRAMES;
        for (uint32_t i = 0; i < frames; ++i) {
            int16_t sample = phase < AUDIO__I2S_SAMPLE_RATE / 2u ? amplitude : (int16_t)-amplitude;
            samples[i * 2u] = sample;
            samples[i * 2u + 1u] = sample;
            phase += params->frequency_hz;
            phase %= AUDIO__I2S_SAMPLE_RATE;
        }
        result = audio__i2s_write_locked(samples, frames);
        if (result != BRUCE_OK) break;
        frames_remaining -= frames;
    }

    /* The tone's last written sample almost never lands on a zero crossing
     * (phase is wherever it happens to be when frames_remaining hits 0), so
     * without this the DMA ring's steady-state content after this call would
     * be the tail of a square wave sitting at the amplitude peak, not
     * silence -- and since the channel is no longer torn down between calls,
     * that would keep looping audibly instead of stopping. Flushing the
     * whole ring with true zero closes the waveform out cleanly and leaves
     * the bus driving real silence until the next tone or stream. */
    bruce_result_t silence_result = audio__i2s_flush_silence_locked();
    return result == BRUCE_OK ? silence_result : result;
}

/* Tears down whatever stream is currently open, best-effort. Called both
 * from audio__stream_close() and from the automatic per-process resource
 * cleanup below; must be called with s_audio_mutex held. */
static void audio__stream_teardown_locked(void) {
    s_stream_open = false;
    atomic_store(&s_stream_read_cursor, 0);
    atomic_store(&s_stream_write_cursor, 0);
    if (s_i2s_tx_ready) { (void)audio__i2s_flush_silence_locked(); }
    heap_caps_free(s_stream_ring);
    s_stream_ring = NULL;
    s_stream_owner = BRUCE_PROCESS_ID_INVALID;
    s_stream_resource = BRUCE_RESOURCE_ID_INVALID;
    s_stream_channels = 0;
}
#else
static void audio__stream_teardown_locked(void) {
    s_stream_open = false;
    s_stream_owner = BRUCE_PROCESS_ID_INVALID;
    s_stream_resource = BRUCE_RESOURCE_ID_INVALID;
    s_stream_channels = 0;
}
#endif

static bruce_result_t audio__play(const audio__tone_params_t *params) {
    /* Force-kill must not delete this task while it holds s_audio_mutex or has
     * the I2S/LEDC peripheral enabled: that would leave the speaker driving its
     * last DMA buffer/duty cycle forever and deadlock every later audio__tone()
     * call on the now-unreleasable mutex. operation_begin/end makes
     * process__kill() wait for audio__play() to finish its own teardown first. */
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    vTaskDelay(pdMS_TO_TICKS(params->duration_ms));
    bruce_result_t result = BRUCE_OK;
#elif CONFIG_BRUCE_AUDIO_BACKEND_I2S
    bruce_result_t result = audio__play_i2s(params);
#elif CONFIG_BRUCE_AUDIO_BACKEND_LEDC_BUZZER
    ledc_timer_bit_t resolution = params->frequency_hz <= AUDIO__MAX_FREQUENCY_13_BIT_HZ
                                      ? LEDC_TIMER_13_BIT
                                      : LEDC_TIMER_10_BIT;
    uint32_t half_duty = resolution == LEDC_TIMER_13_BIT ? 4096u : 512u;
    ledc_timer_config_t timer = {
        .speed_mode = AUDIO__LEDC_MODE,
        .duty_resolution = resolution,
        .timer_num = AUDIO__LEDC_TIMER,
        .freq_hz = params->frequency_hz,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = AUDIO__OUTPUT_GPIO,
        .speed_mode = AUDIO__LEDC_MODE,
        .channel = AUDIO__LEDC_CHANNEL,
        .timer_sel = AUDIO__LEDC_TIMER,
        .duty = (half_duty * params->volume) / 100u,
        .hpoint = 0,
    };
    bruce_result_t result = BRUCE_OK;
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) {
        result = BRUCE_ERR_INTERNAL;
    } else {
        vTaskDelay(pdMS_TO_TICKS(params->duration_ms));
        ledc_stop(AUDIO__LEDC_MODE, AUDIO__LEDC_CHANNEL, 0);
    }
    ledc_timer_rst(AUDIO__LEDC_MODE, AUDIO__LEDC_TIMER);
#else
    (void)params;
    bruce_result_t result = BRUCE_OK;
#endif

    xSemaphoreGive(s_audio_mutex);
    process_registry__operation_end();
    return result;
}

static void audio__tone_task(void *context) {
    audio__tone_params_t params = *(audio__tone_params_t *)context;
    free(context);
    (void)audio__play(&params);
    portENTER_CRITICAL(&s_audio_init_mux);
    s_async_tone_count--;
    portEXIT_CRITICAL(&s_audio_init_mux);
    vTaskDelete(NULL);
}

bruce_result_t audio__tone(uint32_t frequency_hz, uint32_t duration_ms, bool non_blocking) {
    if (frequency_hz < BRUCE_AUDIO_MIN_FREQUENCY_HZ || frequency_hz > BRUCE_AUDIO_MAX_FREQUENCY_HZ ||
        duration_ms == 0 || duration_ms > BRUCE_AUDIO_MAX_TONE_DURATION_MS) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bool enabled = false;
    int volume = 0;
    config__get_audio_settings(&enabled, &volume);
    if (!enabled || volume <= 0) return BRUCE_OK;
    if (volume > 100) volume = 100;

    audio__tone_params_t params = {
        .frequency_hz = frequency_hz,
        .duration_ms = duration_ms,
        .volume = (uint32_t)volume,
    };
    if (!non_blocking) return audio__play(&params);

    portENTER_CRITICAL(&s_audio_init_mux);
    if (s_async_tone_count >= AUDIO__MAX_ASYNC_TONES) {
        portEXIT_CRITICAL(&s_audio_init_mux);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    s_async_tone_count++;
    portEXIT_CRITICAL(&s_audio_init_mux);

    audio__tone_params_t *task_params = malloc(sizeof(*task_params));
    if (task_params == NULL) {
        portENTER_CRITICAL(&s_audio_init_mux);
        s_async_tone_count--;
        portEXIT_CRITICAL(&s_audio_init_mux);
        return BRUCE_ERR_NO_MEMORY;
    }
    *task_params = params;
    if (xTaskCreate(audio__tone_task, "audio_tone", 2048, task_params, 4, NULL) != pdPASS) {
        free(task_params);
        portENTER_CRITICAL(&s_audio_init_mux);
        s_async_tone_count--;
        portEXIT_CRITICAL(&s_audio_init_mux);
        return BRUCE_ERR_NO_MEMORY;
    }
    return BRUCE_OK;
}

#if CONFIG_BRUCE_AUDIO_BACKEND_I2S && !CONFIG_BRUCE_QEMU_TEST_MODE
#define AUDIO__STREAM_SAMPLE_RATE_HZ AUDIO__I2S_SAMPLE_RATE
#else
/* No PCM-capable backend: audio__stream_write() below is a silent no-op, but
 * a caller (e.g. an emulated sound chip) still needs a nonzero rate to pace
 * its own synthesis against. */
#define AUDIO__STREAM_SAMPLE_RATE_HZ 22050u
#endif

uint32_t audio__stream_sample_rate(void) { return AUDIO__STREAM_SAMPLE_RATE_HZ; }

/* Automatic per-process cleanup: runs if the stream's owner exits or is
 * force-killed without calling audio__stream_close() itself. See the
 * warning on bruce_process_resource_cleanup_t (core/process/process.h) --
 * this must not block on anything other than briefly-held locks, which is
 * exactly what every other process_registry__resource_register() cleanup in
 * Core (spi__cleanup, storage__file_cleanup, ...) already does. */
static void audio__stream_cleanup(void *context) {
    (void)context;
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    audio__stream_teardown_locked();
    xSemaphoreGive(s_audio_mutex);
}

bruce_result_t audio__stream_open(uint8_t channels) {
    if (channels != 1 && channels != 2) return BRUCE_ERR_INVALID_ARGUMENT;

    /* Same force-kill protection as audio__play(): a stream write blocks on
     * the mutex/peripheral, and force-kill must wait for that to finish
     * before deleting the task out from under it. */
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);

    bruce_result_t result = BRUCE_OK;
    if (s_stream_open) {
        result = BRUCE_ERR_BUSY;
    } else {
#if CONFIG_BRUCE_AUDIO_BACKEND_I2S && !CONFIG_BRUCE_QEMU_TEST_MODE
        s_stream_ring = heap_caps_malloc(
            AUDIO__STREAM_RING_FRAMES * 2u * sizeof(*s_stream_ring), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
        if (s_stream_ring == NULL) {
            result = BRUCE_ERR_NO_MEMORY;
        } else {
            result = audio__i2s_ensure_channel_locked();
            if (result != BRUCE_OK) {
                heap_caps_free(s_stream_ring);
                s_stream_ring = NULL;
            }
        }
#endif
        if (result == BRUCE_OK) {
#if CONFIG_BRUCE_AUDIO_BACKEND_I2S && !CONFIG_BRUCE_QEMU_TEST_MODE
            atomic_store(&s_stream_read_cursor, 0);
            atomic_store(&s_stream_write_cursor, 0);
#endif
            s_stream_open = true;
            s_stream_owner = process__current_id();
            s_stream_resource = process_registry__resource_register(audio__stream_cleanup, NULL);
            s_stream_channels = channels;
        }
    }

    xSemaphoreGive(s_audio_mutex);
    process_registry__operation_end();
    return result;
}

size_t audio__stream_writable_frames(void) {
    if (!s_stream_open || s_stream_owner != process__current_id()) return 0;
#if CONFIG_BRUCE_AUDIO_BACKEND_I2S && !CONFIG_BRUCE_QEMU_TEST_MODE
    uint32_t write_cursor = atomic_load_explicit(&s_stream_write_cursor, memory_order_relaxed);
    uint32_t read_cursor = atomic_load_explicit(&s_stream_read_cursor, memory_order_acquire);
    uint32_t queued = write_cursor - read_cursor;
    return queued >= AUDIO__STREAM_RING_FRAMES ? 0 : AUDIO__STREAM_RING_FRAMES - queued;
#else
    return SIZE_MAX;
#endif
}

size_t audio__stream_write(const int16_t *samples, size_t frame_count) {
    if (samples == NULL || frame_count == 0 || !s_stream_open || s_stream_owner != process__current_id()) return 0;
    bool enabled = false;
    int volume = 0;
    config__get_audio_settings(&enabled, &volume);
    if (!enabled || volume <= 0) return frame_count;
#if CONFIG_BRUCE_AUDIO_BACKEND_I2S && !CONFIG_BRUCE_QEMU_TEST_MODE
    if (volume > 100) volume = 100;
    size_t writable = audio__stream_writable_frames();
    if (frame_count < writable) writable = frame_count;
    bool stereo = s_stream_channels == 2;
    uint32_t write_cursor = atomic_load_explicit(&s_stream_write_cursor, memory_order_relaxed);
    for (size_t i = 0; i < writable; ++i) {
        uint32_t ring_index = (write_cursor + i) % AUDIO__STREAM_RING_FRAMES;
        int32_t left = stereo ? samples[i * 2u] : samples[i];
        int32_t right = stereo ? samples[i * 2u + 1u] : left;
        s_stream_ring[ring_index * 2u] = (int16_t)((left * volume) / 100);
        s_stream_ring[ring_index * 2u + 1u] = (int16_t)((right * volume) / 100);
    }
    atomic_store_explicit(&s_stream_write_cursor, write_cursor + writable, memory_order_release);
    return writable;
#else
    return frame_count;
#endif
}

bruce_result_t audio__stream_close(void) {
    if (!process_registry__operation_begin()) return BRUCE_ERR_CANCELLED;
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);

    bruce_result_t result = BRUCE_OK;
    bruce_resource_id_t resource = BRUCE_RESOURCE_ID_INVALID;
    if (!s_stream_open || s_stream_owner != process__current_id()) {
        result = BRUCE_ERR_INVALID_STATE;
    } else {
        resource = s_stream_resource;
        audio__stream_teardown_locked();
    }

    xSemaphoreGive(s_audio_mutex);
    if (result == BRUCE_OK && resource != BRUCE_RESOURCE_ID_INVALID) {
        process_registry__resource_release(resource);
    }
    process_registry__operation_end();
    return result;
}
