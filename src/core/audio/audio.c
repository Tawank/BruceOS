#include "core_sdk/audio.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config/config.h"
#include "core/process/process.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "esp_audio_simple_player.h"
#include "esp_gmf_err.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "bruce_audio"

#if CONFIG_BRUCE_AUDIO_BACKEND_I2S
#if CONFIG_BRUCE_AUDIO_ES8311_CODEC
#include "core_sdk/i2c.h"
#endif
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
#define AUDIO__PRODUCER_TASK_PRIORITY (tskIDLE_PRIORITY + 4u)
/* esp_audio_simple_player's own decode task (MP3 decode + resample +
 * channel/bit conversion) is CPU-heavy enough to stay runnable back-to-back
 * for whole seconds at a time once its input ring is primed -- at
 * AUDIO__PRODUCER_TASK_PRIORITY (reserved for work that must preempt
 * everything to protect the I2S ring's ~85ms headroom) that starved the
 * idle task (task watchdog) and the UI/shell below it. It only needs to
 * stay ahead of audio__mp3_play_blocking()'s own drain loop on average, not
 * preempt it, so it runs below ordinary foreground-app priority instead. */
#define AUDIO__MP3_DECODE_TASK_PRIORITY (tskIDLE_PRIORITY + 1u)

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

#if CONFIG_BRUCE_AUDIO_ES8311_CODEC
static bruce_result_t audio__es8311_enable(void) {
    static const uint8_t configuration[][2] = {
        {0x00, 0x80}, /* Reset / CSM power on. */
        {0x01, 0xB5}, /* Clock source is BCLK. */
        {0x02, 0x18},
        {0x0D, 0x01}, /* Power analog circuitry. */
        {0x12, 0x00}, /* Power up DAC. */
        {0x13, 0x10}, /* Route DAC to headphone driver. */
        {0x32, 0xBF}, /* DAC volume: 0 dB. */
        {0x37, 0x08}, /* Bypass DAC equalizer. */
    };
    bruce_i2c_bus_config_t bus_config = {
        .port = BRUCE_I2C_PORT_AUTO,
        .sda = CONFIG_BRUCE_BOARD_I2C_SDA_GPIO,
        .scl = CONFIG_BRUCE_BOARD_I2C_SCL_GPIO,
        .clock_hz = CONFIG_BRUCE_BOARD_I2C_FREQ_HZ,
        .enable_internal_pullups = true,
    };
    bruce_i2c_id_t bus = BRUCE_I2C_ID_INVALID;
    bruce_result_t result = i2c__open(&bus_config, &bus);
    if (result != BRUCE_OK) return result;
    for (size_t i = 0; i < sizeof(configuration) / sizeof(configuration[0]); ++i) {
        result = i2c__write(bus, 0x18, configuration[i], sizeof(configuration[i]), 100);
        if (result != BRUCE_OK) break;
    }
    (void)i2c__close(bus);
    return result;
}
#endif

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
static int16_t s_i2s_output_buffer[AUDIO__I2S_BUFFER_FRAMES * 2u];

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

#if CONFIG_BRUCE_AUDIO_ES8311_CODEC
    bruce_result_t codec_result = audio__es8311_enable();
    if (codec_result != BRUCE_OK) return codec_result;
#endif

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
            }, },
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
    if (i2s_channel_write(s_i2s_tx_channel, interleaved_stereo, bytes, &bytes_written, portMAX_DELAY) !=
            ESP_OK ||
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
    memset(s_i2s_output_buffer, 0, sizeof(s_i2s_output_buffer));
    bruce_result_t result = BRUCE_OK;
    for (uint32_t frames_written = 0; frames_written < AUDIO__I2S_DMA_RING_FRAMES && result == BRUCE_OK;
         frames_written += AUDIO__I2S_BUFFER_FRAMES) {
        result = audio__i2s_write_locked(s_i2s_output_buffer, AUDIO__I2S_BUFFER_FRAMES);
    }
    return result;
}

static bruce_result_t audio__play_i2s(const audio__tone_params_t *params) {
    bruce_result_t result = audio__i2s_ensure_channel_locked();
    if (result != BRUCE_OK) return result;

    int16_t amplitude = (int16_t)((INT16_MAX * params->volume) / 100u);
    uint32_t phase = 0;
    uint32_t frames_remaining = (AUDIO__I2S_SAMPLE_RATE * params->duration_ms) / 1000u;
    while (frames_remaining > 0) {
        uint32_t frames =
            frames_remaining < AUDIO__I2S_BUFFER_FRAMES ? frames_remaining : AUDIO__I2S_BUFFER_FRAMES;
        for (uint32_t i = 0; i < frames; ++i) {
            int16_t sample = phase < AUDIO__I2S_SAMPLE_RATE / 2u ? amplitude : (int16_t)-amplitude;
            s_i2s_output_buffer[i * 2u] = sample;
            s_i2s_output_buffer[i * 2u + 1u] = sample;
            phase += params->frequency_hz;
            phase %= AUDIO__I2S_SAMPLE_RATE;
        }
        result = audio__i2s_write_locked(s_i2s_output_buffer, frames);
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
    ledc_timer_bit_t resolution =
        params->frequency_hz <= AUDIO__MAX_FREQUENCY_13_BIT_HZ ? LEDC_TIMER_13_BIT : LEDC_TIMER_10_BIT;
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
    if (xTaskCreate(audio__tone_task, "audio_tone", 4096, task_params, AUDIO__PRODUCER_TASK_PRIORITY, NULL) !=
        pdPASS) {
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
    if (samples == NULL || frame_count == 0 || !s_stream_open || s_stream_owner != process__current_id())
        return 0;
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

/* File playback (audio__file_play() and friends below) -- see
 * core_sdk/audio.h's own doc comments for the concurrency model.
 * audio__file_play_blocking() below is the actual decode/resample/stream
 * loop; it owns the storage file handle it opens, so it must run to
 * completion on a single process's task (a handle can't cross processes --
 * see core_sdk/storage.h). audio__file_play() itself just validates the
 * path quickly and hands the real work to a dedicated background process
 * via process_registry__create(), so callers get a simple, non-blocking
 * call without needing to know any of that -- same shape as
 * `ffplay song.wav &` in the shell, just done for them. pause/seek/stop are
 * request flags audio__file_play_blocking()'s own loop polls, since they're
 * meant to be called from a different process while that one plays.
 * Guarded by s_audio_mutex like the rest of this file. The spawned process
 * is created with built_in=true (same as any other built-in command's own
 * process), which permission__check() always grants every permission to --
 * for now, any process can play any audio file it can already read, no
 * extra permission prompt. (app_runner__spawn_loader_process_owned() was
 * tried first, but it always creates a non-built-in process keyed by its
 * permission_key, which is right for untrusted ELF/JS loader content but
 * meant this got treated as a brand new, never-granted app and prompted
 * for storage permission on real hardware -- process_registry__create() is
 * available here because audio.c is a Core module, not a sandboxed
 * loader.) */
#define AUDIO__FILE_READ_FRAMES 512u
#define AUDIO__FILE_PLAY_TASK_STACK_BYTES 8192u

typedef struct {
    bruce_file_id_t file;
    uint16_t channels;
    uint16_t bits_per_sample; /* 8 (unsigned PCM) or 16 (signed PCM) */
    uint32_t sample_rate;
    uint32_t data_start;    /* file offset where the "data" chunk's bytes begin, for seeking */
    uint32_t data_size;     /* total bytes in the "data" chunk */
    uint32_t data_position; /* bytes into the data chunk already consumed */
} audio__wav_t;

typedef enum {
    AUDIO__FILE_FORMAT_NONE = 0,
    AUDIO__FILE_FORMAT_WAV,
    AUDIO__FILE_FORMAT_MP3,
} audio__file_format_t;

static volatile bool s_file_open;
static volatile bool s_file_playing;
static volatile bool s_file_stop_requested;
static volatile bool s_file_seek_requested;
static volatile uint32_t s_file_seek_target_ms;
static bruce_resource_id_t s_file_resource;
static audio__wav_t s_file_wav;
static audio__file_format_t s_file_format;
static esp_asp_handle_t s_mp3_handle;

/* Reports whether the process audio__file_play() just spawned actually
 * opened the file, for audio__file_play() itself to wait on below -- see
 * its own comment on why this is needed (in short: a caller polling
 * audio__file_is_open() right after audio__file_play() returns can't tell
 * "hasn't opened yet" apart from "already finished", and MP3's own setup
 * -- reading the whole file into RAM, building the esp_audio_simple_player
 * pipeline -- takes long enough that losing that race is the common case,
 * not an edge case). s_file_open_token identifies which audio__file_play()
 * call a given s_file_open_result belongs to, since only one play session
 * is tracked by s_file_open/s_file_format/etc. at a time but a new call can
 * be made (and get BRUCE_ERR_BUSY from audio__stream_open()) before a
 * slow-to-fail previous one has published its own result. */
static volatile uint32_t s_file_open_token;
static volatile uint32_t s_file_open_result_token;
static volatile bruce_result_t s_file_open_result;

/* True from the moment audio__file_play() spawns its background process
 * until that process either opens the file (s_file_open becomes true) or
 * fails (audio__file_publish_open_result() below). audio__file_play() itself
 * no longer waits for either -- it returns right away, matching its own
 * documented non-blocking contract -- so a caller that immediately polls
 * audio__file_is_open()/_is_playing() (ffplay's wait loop, for one) needs
 * some way to see "still opening" rather than mistaking that gap for
 * "already finished" and giving up instantly. audio__file_is_open() folds
 * this in below. */
static volatile bool s_file_opening;

/* Must NOT be called with s_audio_mutex held. */
static void audio__file_publish_open_result(uint32_t token, bruce_result_t result) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    s_file_open_result_token = token;
    s_file_open_result = result;
    s_file_opening = false;
    xSemaphoreGive(s_audio_mutex);
}

static bool audio__file_read_exact(bruce_file_id_t file, void *buffer, size_t size) {
    size_t got = 0;
    return storage__read(file, buffer, size, &got) == BRUCE_OK && got == size;
}

/* Opens `path` as an 8- or 16-bit PCM .wav file (mono or stereo, any sample
 * rate). Only the common cases are handled: PCM format tag (1), 8 bits/
 * sample (unsigned) or 16 bits/sample (signed). Anything else (a
 * compressed format, 24/32-bit samples, a missing or malformed header, or
 * just not a .wav at all) returns false -- this is a minimal reader, not a
 * general RIFF/WAV parser. */
static bool audio__wav_open(const char *path, audio__wav_t *out) {
    bruce_file_id_t file;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return false;

    uint8_t riff_header[12];
    if (!audio__file_read_exact(file, riff_header, sizeof(riff_header)) || memcmp(riff_header, "RIFF", 4) != 0 ||
        memcmp(riff_header + 8, "WAVE", 4) != 0) {
        storage__close(file);
        return false;
    }

    bool have_fmt = false, have_data = false;
    uint16_t audio_format = 0, channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0, data_start = 0;
    while (!have_data) {
        uint8_t chunk_header[8];
        if (!audio__file_read_exact(file, chunk_header, sizeof(chunk_header))) break;
        uint32_t chunk_size = (uint32_t)chunk_header[4] | ((uint32_t)chunk_header[5] << 8) |
                               ((uint32_t)chunk_header[6] << 16) | ((uint32_t)chunk_header[7] << 24);
        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || !audio__file_read_exact(file, fmt, sizeof(fmt))) break;
            audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));
            channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            sample_rate =
                (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits_per_sample = (uint16_t)(fmt[14] | (fmt[15] << 8));
            uint32_t skip = chunk_size - (uint32_t)sizeof(fmt);
            if (skip > 0 && storage__seek(file, (int64_t)skip, SEEK_CUR, NULL) != BRUCE_OK) break;
            have_fmt = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_size = chunk_size;
            uint64_t position = 0;
            if (storage__seek(file, 0, SEEK_CUR, &position) != BRUCE_OK) break;
            data_start = (uint32_t)position;
            have_data = true;
        } else {
            uint32_t skip = chunk_size + (chunk_size & 1u); /* chunks are word-aligned */
            if (storage__seek(file, (int64_t)skip, SEEK_CUR, NULL) != BRUCE_OK) break;
        }
    }

    if (!have_fmt || !have_data || audio_format != 1 || (bits_per_sample != 8 && bits_per_sample != 16) ||
        (channels != 1 && channels != 2) || sample_rate == 0) {
        storage__close(file);
        return false;
    }
    out->file = file;
    out->channels = channels;
    out->bits_per_sample = bits_per_sample;
    out->sample_rate = sample_rate;
    out->data_start = data_start;
    out->data_size = data_size;
    out->data_position = 0;
    return true;
}

/* Must be called with s_audio_mutex held. Format-specific teardown: a WAV
 * play owns a storage handle directly; an MP3 play owns an
 * esp_audio_simple_player instance instead (see the MP3 section below for
 * why its storage handle and RAM buffers are never held past the preload
 * step, so there's nothing format-specific to release for those here). Also
 * reached from the force-kill resource-cleanup path (audio__file_cleanup()
 * below), so esp_audio_simple_player_stop()/_destroy() briefly blocking
 * while its own task unwinds is an accepted tradeoff of integrating a
 * stateful third-party player, same as storage__close() doing real flash
 * I/O here for the WAV case. */
static void audio__file_teardown_locked(void) {
    if (s_file_format == AUDIO__FILE_FORMAT_WAV && s_file_open) {
        storage__close(s_file_wav.file);
    } else if (s_file_format == AUDIO__FILE_FORMAT_MP3 && s_mp3_handle != NULL) {
        (void)esp_audio_simple_player_stop(s_mp3_handle);
        (void)esp_audio_simple_player_destroy(s_mp3_handle);
        s_mp3_handle = NULL;
    }
    s_file_open = false;
    s_file_playing = false;
    s_file_stop_requested = false;
    s_file_seek_requested = false;
    s_file_format = AUDIO__FILE_FORMAT_NONE;
    s_file_resource = BRUCE_RESOURCE_ID_INVALID;
    memset(&s_file_wav, 0, sizeof(s_file_wav));
}

/* Automatic per-process cleanup -- see audio__stream_cleanup()'s identical
 * comment above on why this must stay this simple. */
static void audio__file_cleanup(void *context) {
    (void)context;
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    audio__file_teardown_locked();
    xSemaphoreGive(s_audio_mutex);
}

typedef struct {
    bool stop;
    bool playing;
    bool seek;
    uint32_t seek_ms;
} audio__file_poll_t;

/* Every s_audio_mutex critical section below is its own short
 * process_registry__operation_begin()/_end() pair rather than one spanning
 * audio__file_play()'s whole (potentially minutes-long) loop -- see
 * audio__stream_open()'s identical comment on why: force-kill waits out
 * whatever operation is in progress before deleting the task, so wrapping
 * the entire loop in one pair would make a stuck/long-playing file
 * unkillable for its whole duration, and *not* wrapping each mutex section
 * at all would risk force-kill deleting this task mid-section and leaving
 * s_audio_mutex locked forever. Returns false (nothing read) if shutdown
 * has already started. */
static bool audio__file_poll_locked(audio__file_poll_t *out) {
    if (!process_registry__operation_begin()) return false;
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    out->stop = s_file_stop_requested;
    out->playing = s_file_playing;
    out->seek = s_file_seek_requested;
    out->seek_ms = s_file_seek_target_ms;
    if (out->seek) s_file_seek_requested = false;
    xSemaphoreGive(s_audio_mutex);
    process_registry__operation_end();
    return true;
}

/* Publishes `data_position` (already advanced in audio__file_play()'s own
 * local `wav` copy) to the shared s_file_wav that
 * audio__file_position_ms()/_duration_ms() read from another process --
 * best-effort: silently skipped if shutdown has already started, same as
 * audio__file_poll_locked() above. */
static void audio__file_publish_position_locked(uint32_t data_position) {
    if (!process_registry__operation_begin()) return;
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    s_file_wav.data_position = data_position;
    xSemaphoreGive(s_audio_mutex);
    process_registry__operation_end();
}

/* Reads one sample at `bits_per_sample` from `raw` and returns it as signed
 * 16-bit PCM. 8-bit WAV samples are unsigned (128 = silence), so they're
 * recentered and scaled up; 16-bit samples are already signed and are just
 * decoded little-endian. */
static int16_t audio__wav_sample_to_pcm16(const uint8_t *raw, uint16_t bits_per_sample) {
    if (bits_per_sample == 8) return (int16_t)(((int)raw[0] - 128) * 256);
    return (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
}

static bruce_result_t audio__file_play_blocking(const char *path, uint32_t token) {
    audio__wav_t wav;
    if (!audio__wav_open(path, &wav)) {
        audio__file_publish_open_result(token, BRUCE_ERR_UNSUPPORTED);
        return BRUCE_ERR_UNSUPPORTED;
    }

    bruce_result_t opened = audio__stream_open(wav.channels);
    if (opened != BRUCE_OK) {
        storage__close(wav.file);
        audio__file_publish_open_result(token, opened);
        return opened;
    }

    if (!process_registry__operation_begin()) {
        (void)audio__stream_close();
        storage__close(wav.file);
        audio__file_publish_open_result(token, BRUCE_ERR_CANCELLED);
        return BRUCE_ERR_CANCELLED;
    }
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    s_file_wav = wav;
    s_file_format = AUDIO__FILE_FORMAT_WAV;
    s_file_open = true;
    s_file_playing = true;
    s_file_stop_requested = false;
    s_file_seek_requested = false;
    s_file_resource = process_registry__resource_register(audio__file_cleanup, NULL);
    s_file_open_result_token = token;
    s_file_open_result = BRUCE_OK;
    s_file_opening = false;
    xSemaphoreGive(s_audio_mutex);
    process_registry__operation_end();

    uint32_t out_rate = audio__stream_sample_rate();
    size_t sample_bytes = (size_t)wav.bits_per_sample / 8u;
    size_t frame_bytes = (size_t)wav.channels * sample_bytes;
    uint8_t *in_buf = malloc(AUDIO__FILE_READ_FRAMES * frame_bytes);
    int16_t *out_buf = malloc(AUDIO__FILE_READ_FRAMES * (size_t)wav.channels * sizeof(int16_t));
    bool out_of_memory = in_buf == NULL || out_buf == NULL;

    size_t in_count = 0, in_pos = 0;
    int16_t current_frame[2] = {0, 0};
    uint32_t acc = 0;
    bool done = out_of_memory;
    while (!done) {
        if (process__current_signal() != 0) break;

        audio__file_poll_t poll;
        if (!audio__file_poll_locked(&poll)) break; /* device shutting down */

        if (poll.stop) break;
        if (poll.seek) {
            uint64_t target_frame = ((uint64_t)poll.seek_ms * wav.sample_rate) / 1000u;
            uint64_t byte_offset = target_frame * frame_bytes;
            if (byte_offset > wav.data_size) byte_offset = wav.data_size;
            if (storage__seek(wav.file, (int64_t)(wav.data_start + byte_offset), SEEK_SET, NULL) == BRUCE_OK) {
                wav.data_position = (uint32_t)byte_offset;
                audio__file_publish_position_locked(wav.data_position);
                in_count = 0;
                in_pos = 0;
                acc = 0;
            }
        }
        if (!poll.playing) {
            (void)runtime__sleep(20u);
            continue;
        }

        if (in_pos >= in_count) {
            size_t want = AUDIO__FILE_READ_FRAMES * frame_bytes;
            size_t remaining = wav.data_size - wav.data_position;
            if (want > remaining) want = remaining;
            size_t got = 0;
            if (want == 0 || storage__read(wav.file, in_buf, want, &got) != BRUCE_OK || got == 0) break;
            wav.data_position += (uint32_t)got;
            audio__file_publish_position_locked(wav.data_position);
            in_count = got / frame_bytes;
            in_pos = 0;
        }

        size_t out_count = 0;
        while (in_pos < in_count && out_count < AUDIO__FILE_READ_FRAMES) {
            acc += wav.sample_rate;
            while (acc >= out_rate && in_pos < in_count) {
                acc -= out_rate;
                const uint8_t *frame = in_buf + in_pos * frame_bytes;
                current_frame[0] = audio__wav_sample_to_pcm16(frame, wav.bits_per_sample);
                if (wav.channels == 2) {
                    current_frame[1] = audio__wav_sample_to_pcm16(frame + sample_bytes, wav.bits_per_sample);
                }
                in_pos++;
            }
            out_buf[out_count * wav.channels] = current_frame[0];
            if (wav.channels == 2) out_buf[out_count * wav.channels + 1] = current_frame[1];
            out_count++;
        }

        size_t written = 0;
        while (written < out_count && process__current_signal() == 0) {
            size_t copied = audio__stream_write(out_buf + written * wav.channels, out_count - written);
            if (copied == 0) {
                if (runtime__sleep(10u) != BRUCE_OK) break;
                continue;
            }
            written += copied;
        }
    }

    free(in_buf);
    free(out_buf);

    bruce_resource_id_t resource = BRUCE_RESOURCE_ID_INVALID;
    if (process_registry__operation_begin()) {
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        resource = s_file_resource;
        audio__file_teardown_locked();
        xSemaphoreGive(s_audio_mutex);
        process_registry__operation_end();
    }
    (void)audio__stream_close();
    if (resource != BRUCE_RESOURCE_ID_INVALID) process_registry__resource_release(resource);
    return BRUCE_OK;
}

/* Context for audio__file_play_process_entry()/audio__mp3_play_process_entry()
 * below -- a heap copy of the path (the caller's own stack/path buffer
 * won't outlive this call once audio__file_play() returns) plus the open
 * token audio__file_play() generated for this call, so the blocking
 * function can publish its result under the right token (see
 * s_file_open_token's own comment above). Freed by
 * audio__file_play_process_cleanup() regardless of how the spawned process
 * ends (normal return, force-kill, or never starting at all). */
typedef struct {
    char *path;
    uint32_t token;
} audio__file_play_context_t;

static int audio__file_play_process_entry(void *context) {
    audio__file_play_context_t *ctx = context;
    (void)audio__file_play_blocking(ctx->path, ctx->token);
    return 0;
}

static void audio__file_play_process_cleanup(void *context) {
    audio__file_play_context_t *ctx = context;
    /* Covers force-kill during the open window: the process never got to
     * call audio__file_publish_open_result() (or anything else) for its own
     * token, so without this s_file_opening would stay stuck true forever. */
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    if (s_file_open_result_token != ctx->token) s_file_opening = false;
    xSemaphoreGive(s_audio_mutex);
    free(ctx->path);
    free(ctx);
}

/* MP3 playback -- esp_audio_simple_player, "raw://" input scheme ---------
 *
 * esp_audio_simple_player decodes on its OWN internal FreeRTOS task, not
 * this process's, so its in/out callbacks can't call
 * storage__read()/audio__stream_write() directly: both are process-owned by
 * whichever process opened the handle/stream (see
 * storage__file_owned_by_caller_locked() in core/storage/storage.c and
 * audio__stream_write() above), and a task process_registry never created
 * resolves to BRUCE_PROCESS_ID_INVALID, so it gets rejected -- the exact
 * problem process_registry__create(built_in=true) above already solved one
 * level up for this process's own task, now one level deeper.
 *
 * Worked around by:
 *   - a small ring of still-compressed bytes (audio__mp3_source_t), filled
 *     from storage__read() by this process's own task (audio__mp3_source_fill(),
 *     called from audio__mp3_play_blocking()'s own loop below, interleaved
 *     with draining the output ring). The "in" callback just drains that
 *     ring -- a plain memcpy, no storage call, no process-identity check,
 *     safe from any task -- and waits (vTaskDelay) for more if it's
 *     temporarily empty but the file isn't finished yet. This keeps RAM use
 *     flat regardless of file size, unlike reading the whole file into RAM
 *     up front: most boards this targets have no PSRAM at all (confirmed
 *     for e.g. m5stack-cardputer/-adv, both plain ESP32-S3FN8), so even a
 *     ~700KB MP3 would never fit as one allocation on internal RAM alongside
 *     everything else already running.
 *   - the "out" callback copies decoded PCM into a small intermediate ring
 *     (its own plain FreeRTOS mutex, no process-ownership concept), which
 *     this process's own task (audio__mp3_play_blocking()'s loop) drains
 *     into audio__stream_write() the normal way.
 * Both rings come from memory__external_malloc_writable() (PSRAM when
 * available, otherwise internal RAM, never flash-backed swap -- see
 * core_sdk/memory.h), and are small/fixed-size regardless of file size.
 *
 * Position/duration/seek are not tracked for MP3 -- see audio.h. */
/* Trimmed from 2048/16384 (8KB/16KB): these boards have no PSRAM, so both
 * rings live in internal RAM (memory__external_malloc_writable() can't put
 * them anywhere else -- see its own doc comment), competing for the same
 * pool esp_audio_simple_player's pipeline needs its own one-shot internal
 * allocations from (e.g. the rate converter's ~15KB coefficients matrix,
 * opened once at playback start -- CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE
 * in sdkconfig.defaults). Smaller rings mean less headroom against a slow
 * storage read or a backed-up I2S write before audio audibly stutters, but
 * that's a straight tradeoff against the pipeline failing to open at all. */
#define AUDIO__MP3_PCM_RING_FRAMES 1024u  /* stereo int16 frames, ~4KB */
#define AUDIO__MP3_INPUT_RING_BYTES 8192u /* compressed bytes, ~0.5s at 128kbps */
#define AUDIO__MP3_TRANSFER_FRAMES 256u

typedef struct {
    SemaphoreHandle_t lock;
    uint8_t *ring; /* AUDIO__MP3_INPUT_RING_BYTES long */
    size_t write_pos;
    size_t read_pos;
    size_t filled;
    bruce_file_id_t file;
    size_t file_size;
    size_t total_read; /* bytes pulled from `file` into the ring so far */
} audio__mp3_source_t;

/* Pulls more compressed bytes from storage into the ring, up to whatever
 * free space is contiguous from the current write position (a second call
 * picks up past the wrap, same as a single-producer ring normally works).
 * Only ever called from this process's own task -- the one that actually
 * owns `file`'s storage handle -- never from esp_audio_simple_player's. */
static void audio__mp3_source_fill(audio__mp3_source_t *source) {
    if (source->total_read >= source->file_size) return;
    xSemaphoreTake(source->lock, portMAX_DELAY);
    size_t free_bytes = AUDIO__MP3_INPUT_RING_BYTES - source->filled;
    size_t write_pos = source->write_pos;
    xSemaphoreGive(source->lock);
    if (free_bytes == 0) return;

    size_t contiguous = AUDIO__MP3_INPUT_RING_BYTES - write_pos;
    size_t want = free_bytes < contiguous ? free_bytes : contiguous;
    size_t remaining_in_file = source->file_size - source->total_read;
    if (want > remaining_in_file) want = remaining_in_file;
    if (want == 0) return;

    size_t got = 0;
    if (storage__read(source->file, source->ring + write_pos, want, &got) != BRUCE_OK) return;

    xSemaphoreTake(source->lock, portMAX_DELAY);
    source->write_pos = (source->write_pos + got) % AUDIO__MP3_INPUT_RING_BYTES;
    source->filled += got;
    xSemaphoreGive(source->lock);
    source->total_read += got;
}

/* Runs on esp_audio_simple_player's own task. Waits (briefly, via
 * vTaskDelay -- see audio__mp3_out_cb()'s identical reasoning) for
 * audio__mp3_source_fill() to top the ring up if it's temporarily empty but
 * the file isn't fully read yet; returns less than requested (down to 0) at
 * genuine end-of-file, same as fread(). */
static int audio__mp3_in_cb(uint8_t *data, int data_size, void *ctx) {
    audio__mp3_source_t *source = ctx;
    size_t want = (size_t)data_size;
    size_t got_total = 0;
    while (got_total < want) {
        xSemaphoreTake(source->lock, portMAX_DELAY);
        size_t avail = source->filled;
        size_t chunk = want - got_total;
        if (chunk > avail) chunk = avail;
        size_t contiguous = AUDIO__MP3_INPUT_RING_BYTES - source->read_pos;
        size_t first = chunk < contiguous ? chunk : contiguous;
        if (first > 0) memcpy(data + got_total, source->ring + source->read_pos, first);
        if (chunk > first) memcpy(data + got_total + first, source->ring, chunk - first);
        source->read_pos = (source->read_pos + chunk) % AUDIO__MP3_INPUT_RING_BYTES;
        source->filled -= chunk;
        bool eof = source->total_read >= source->file_size;
        xSemaphoreGive(source->lock);
        got_total += chunk;
        if (chunk == 0) {
            if (eof) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return (int)got_total;
}

typedef struct {
    SemaphoreHandle_t lock;
    int16_t *ring; /* stereo int16 frames, AUDIO__MP3_PCM_RING_FRAMES long */
    size_t write_pos;
    size_t read_pos;
    size_t filled;
    volatile bool finished;
} audio__mp3_pipe_t;

/* Runs on esp_audio_simple_player's own task; forced to stereo 16-bit PCM
 * by the CH_CVT/BIT_CVT sdkconfig defaults, so `data` is always int16 L/R
 * pairs. Blocks (briefly, via vTaskDelay -- not runtime__sleep(), which
 * polls this process's own kill signal, the wrong process to ask from this
 * task) when the intermediate ring is full, applying backpressure to the
 * decoder same as a blocking hardware write would. */
static int audio__mp3_out_cb(uint8_t *data, int data_size, void *ctx) {
    audio__mp3_pipe_t *pipe = ctx;
    const int16_t *samples = (const int16_t *)data;
    size_t frame_count = (size_t)data_size / (2u * sizeof(int16_t));
    size_t written = 0;
    while (written < frame_count) {
        xSemaphoreTake(pipe->lock, portMAX_DELAY);
        size_t free_frames = AUDIO__MP3_PCM_RING_FRAMES - pipe->filled;
        size_t chunk = frame_count - written;
        if (chunk > free_frames) chunk = free_frames;
        for (size_t i = 0; i < chunk; ++i) {
            pipe->ring[pipe->write_pos * 2u] = samples[(written + i) * 2u];
            pipe->ring[pipe->write_pos * 2u + 1u] = samples[(written + i) * 2u + 1u];
            pipe->write_pos = (pipe->write_pos + 1u) % AUDIO__MP3_PCM_RING_FRAMES;
        }
        pipe->filled += chunk;
        written += chunk;
        xSemaphoreGive(pipe->lock);
        if (chunk == 0) vTaskDelay(pdMS_TO_TICKS(5));
    }
    return data_size;
}

/* Runs on esp_audio_simple_player's own task. */
static int audio__mp3_event_cb(esp_asp_event_pkt_t *event, void *ctx) {
    audio__mp3_pipe_t *pipe = ctx;
    if (event->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        memcpy(&state, event->payload, event->payload_size);
        if (state == ESP_ASP_STATE_FINISHED || state == ESP_ASP_STATE_STOPPED ||
            state == ESP_ASP_STATE_ERROR) {
            pipe->finished = true;
        }
    }
    return 0;
}

static bruce_result_t audio__mp3_play_blocking(const char *path, uint32_t token) {
    bruce_file_id_t file;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) {
        ESP_LOGE(TAG, "mp3: could not open %s", path);
        audio__file_publish_open_result(token, BRUCE_ERR_UNSUPPORTED);
        return BRUCE_ERR_UNSUPPORTED;
    }
    uint64_t file_size = 0;
    if (storage__seek(file, 0, SEEK_END, &file_size) != BRUCE_OK || file_size == 0 ||
        storage__seek(file, 0, SEEK_SET, NULL) != BRUCE_OK) {
        ESP_LOGE(TAG, "mp3: could not determine size of %s", path);
        storage__close(file);
        audio__file_publish_open_result(token, BRUCE_ERR_UNSUPPORTED);
        return BRUCE_ERR_UNSUPPORTED;
    }

    const void *input_ring_buffer = memory__external_malloc_writable(AUDIO__MP3_INPUT_RING_BYTES);
    if (input_ring_buffer == NULL) {
        ESP_LOGE(TAG, "mp3: out of memory for input ring");
        storage__close(file);
        audio__file_publish_open_result(token, BRUCE_ERR_NO_MEMORY);
        return BRUCE_ERR_NO_MEMORY;
    }

    const void *pcm_ring_buffer =
        memory__external_malloc_writable(AUDIO__MP3_PCM_RING_FRAMES * 2u * sizeof(int16_t));
    if (pcm_ring_buffer == NULL) {
        ESP_LOGE(TAG, "mp3: out of memory for PCM ring");
        (void)memory__external_free(input_ring_buffer);
        storage__close(file);
        audio__file_publish_open_result(token, BRUCE_ERR_NO_MEMORY);
        return BRUCE_ERR_NO_MEMORY;
    }

    bruce_result_t opened = audio__stream_open(2);
    if (opened != BRUCE_OK) {
        ESP_LOGE(TAG, "mp3: audio__stream_open() failed: %d", (int)opened);
        (void)memory__external_free(pcm_ring_buffer);
        (void)memory__external_free(input_ring_buffer);
        storage__close(file);
        audio__file_publish_open_result(token, opened);
        return opened;
    }

    audio__mp3_source_t source = {0};
    source.lock = xSemaphoreCreateMutex();
    source.ring = (uint8_t *)input_ring_buffer;
    source.file = file;
    source.file_size = (size_t)file_size;

    audio__mp3_pipe_t pipe = {0};
    pipe.lock = xSemaphoreCreateMutex();
    pipe.ring = (int16_t *)pcm_ring_buffer;
    if (source.lock == NULL || pipe.lock == NULL) {
        ESP_LOGE(TAG, "mp3: out of memory for ring locks");
        if (source.lock != NULL) vSemaphoreDelete(source.lock);
        if (pipe.lock != NULL) vSemaphoreDelete(pipe.lock);
        (void)audio__stream_close();
        (void)memory__external_free(pcm_ring_buffer);
        (void)memory__external_free(input_ring_buffer);
        storage__close(file);
        audio__file_publish_open_result(token, BRUCE_ERR_NO_MEMORY);
        return BRUCE_ERR_NO_MEMORY;
    }

    /* Prime the input ring before the decoder's first pull so it doesn't
     * immediately starve waiting on this process's own (much slower,
     * polling) main loop below. */
    while (source.filled < AUDIO__MP3_INPUT_RING_BYTES && source.total_read < source.file_size) {
        size_t filled_before = source.filled;
        audio__mp3_source_fill(&source);
        if (source.filled == filled_before) break; /* a read failed; let the decoder discover EOF */
    }

    esp_asp_cfg_t cfg = {
        .in = {.cb = audio__mp3_in_cb, .user_ctx = &source},
        .out = {.cb = audio__mp3_out_cb, .user_ctx = &pipe},
        .task_prio = (int)AUDIO__MP3_DECODE_TASK_PRIORITY,
        .task_stack = 6 * 1024,
    };
    esp_asp_handle_t handle = NULL;
    esp_gmf_err_t new_result = esp_audio_simple_player_new(&cfg, &handle);
    if (new_result != ESP_GMF_ERR_OK || handle == NULL) {
        ESP_LOGE(TAG, "mp3: esp_audio_simple_player_new() failed: %d", (int)new_result);
        vSemaphoreDelete(source.lock);
        vSemaphoreDelete(pipe.lock);
        (void)audio__stream_close();
        (void)memory__external_free(pcm_ring_buffer);
        (void)memory__external_free(input_ring_buffer);
        storage__close(file);
        audio__file_publish_open_result(token, BRUCE_ERR_NO_MEMORY);
        return BRUCE_ERR_NO_MEMORY;
    }
    (void)esp_audio_simple_player_set_event(handle, audio__mp3_event_cb, &pipe);

    if (!process_registry__operation_begin()) {
        (void)esp_audio_simple_player_destroy(handle);
        vSemaphoreDelete(source.lock);
        vSemaphoreDelete(pipe.lock);
        (void)audio__stream_close();
        (void)memory__external_free(pcm_ring_buffer);
        (void)memory__external_free(input_ring_buffer);
        storage__close(file);
        audio__file_publish_open_result(token, BRUCE_ERR_CANCELLED);
        return BRUCE_ERR_CANCELLED;
    }
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    s_file_format = AUDIO__FILE_FORMAT_MP3;
    s_mp3_handle = handle;
    s_file_open = true;
    s_file_playing = true;
    s_file_stop_requested = false;
    s_file_seek_requested = false;
    s_file_resource = process_registry__resource_register(audio__file_cleanup, NULL);
    s_file_open_result_token = token;
    s_file_open_result = BRUCE_OK;
    s_file_opening = false;
    xSemaphoreGive(s_audio_mutex);
    process_registry__operation_end();

    /* The pipeline's own elements (decoder, resampler, ...) open -- and do
     * their own one-shot internal allocations, e.g. the resampler's
     * coefficients matrix -- asynchronously on esp_audio_simple_player's own
     * task after this call, not synchronously inside it, so a failure there
     * (logged by esp_ae/esp_gmf itself, then delivered to
     * audio__mp3_event_cb() as ESP_GMF_EVENT_STATE_ERROR) can't be caught by
     * checking run_result alone. Logged here so a failure a few lines later
     * in the serial log has the actual free-heap number next to it instead
     * of requiring another guess at how much headroom the pipeline needed. */
    ESP_LOGI(TAG, "mp3: free heap before run(): %u bytes", (unsigned)esp_get_free_heap_size());
    esp_gmf_err_t run_result = esp_audio_simple_player_run(handle, "raw://local/audio.mp3", NULL);
    if (run_result != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "mp3: esp_audio_simple_player_run() failed: %d", (int)run_result);
        pipe.finished = true;
    } else {
        ESP_LOGI(TAG, "mp3: playing %s (%u bytes)", path, (unsigned)source.file_size);
    }

    int16_t transfer[AUDIO__MP3_TRANSFER_FRAMES * 2u];
    bool was_playing = true;
    for (;;) {
        if (process__current_signal() != 0) break;
        audio__file_poll_t poll;
        if (!audio__file_poll_locked(&poll)) break; /* device shutting down */
        if (poll.stop) {
            (void)esp_audio_simple_player_stop(handle);
            break;
        }
        if (poll.playing != was_playing) {
            if (poll.playing) (void)esp_audio_simple_player_resume(handle);
            else (void)esp_audio_simple_player_pause(handle);
            was_playing = poll.playing;
        }

        audio__mp3_source_fill(&source);

        xSemaphoreTake(pipe.lock, portMAX_DELAY);
        size_t want = pipe.filled < AUDIO__MP3_TRANSFER_FRAMES ? pipe.filled : AUDIO__MP3_TRANSFER_FRAMES;
        for (size_t i = 0; i < want; ++i) {
            transfer[i * 2u] = pipe.ring[pipe.read_pos * 2u];
            transfer[i * 2u + 1u] = pipe.ring[pipe.read_pos * 2u + 1u];
            pipe.read_pos = (pipe.read_pos + 1u) % AUDIO__MP3_PCM_RING_FRAMES;
        }
        pipe.filled -= want;
        bool finished = pipe.finished && pipe.filled == 0;
        xSemaphoreGive(pipe.lock);

        if (want > 0) {
            size_t written = 0;
            while (written < want && process__current_signal() == 0) {
                size_t copied = audio__stream_write(transfer + written * 2u, want - written);
                if (copied == 0) {
                    if (runtime__sleep(10u) != BRUCE_OK) break;
                    continue;
                }
                written += copied;
            }
        } else if (finished) {
            break;
        } else {
            (void)runtime__sleep(10u);
        }
    }

    vSemaphoreDelete(source.lock);
    vSemaphoreDelete(pipe.lock);
    (void)memory__external_free(pcm_ring_buffer);
    (void)memory__external_free(input_ring_buffer);
    storage__close(file);

    bruce_resource_id_t resource = BRUCE_RESOURCE_ID_INVALID;
    if (process_registry__operation_begin()) {
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        resource = s_file_resource;
        audio__file_teardown_locked();
        xSemaphoreGive(s_audio_mutex);
        process_registry__operation_end();
    }
    (void)audio__stream_close();
    if (resource != BRUCE_RESOURCE_ID_INVALID) process_registry__resource_release(resource);
    return BRUCE_OK;
}

static int audio__mp3_play_process_entry(void *context) {
    audio__file_play_context_t *ctx = context;
    (void)audio__mp3_play_blocking(ctx->path, ctx->token);
    return 0;
}

static bool audio__path_has_mp3_extension(const char *path) {
    size_t length = strlen(path);
    if (length < 4) return false;
    const char *suffix = path + length - 4;
    return (suffix[0] == '.') && (tolower((unsigned char)suffix[1]) == 'm') &&
           (tolower((unsigned char)suffix[2]) == 'p') && (tolower((unsigned char)suffix[3]) == '3');
}

bruce_result_t audio__file_play(const char *path) {
    if (path == NULL || path[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    size_t path_length = strlen(path);
    if (path_length >= BRUCE_STORAGE_PATH_MAX) return BRUCE_ERR_INVALID_ARGUMENT;

    /* Quick synchronous validation so a bad path/format is reported to the
     * caller immediately instead of silently failing in the background --
     * closed right away because the spawned process below reopens the file
     * itself (a storage handle can't cross processes). Cheap: this only
     * reads the WAV header or probes the MP3's existence/size, never the
     * full audio data. */
    audio__wav_t probe;
    bool is_wav = audio__wav_open(path, &probe);
    if (is_wav) storage__close(probe.file);
    bool is_mp3 = false;
    if (!is_wav && audio__path_has_mp3_extension(path)) {
        bruce_file_id_t probe_file;
        if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &probe_file) == BRUCE_OK) {
            uint64_t size = 0;
            is_mp3 = storage__seek(probe_file, 0, SEEK_END, &size) == BRUCE_OK && size > 0;
            storage__close(probe_file);
        }
    }
    if (!is_wav && !is_mp3) return BRUCE_ERR_UNSUPPORTED;

    audio__file_play_context_t *context = malloc(sizeof(*context));
    if (context == NULL) return BRUCE_ERR_NO_MEMORY;
    context->path = malloc(path_length + 1);
    if (context->path == NULL) {
        free(context);
        return BRUCE_ERR_NO_MEMORY;
    }
    memcpy(context->path, path, path_length + 1);

    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    uint32_t token = ++s_file_open_token;
    /* Set before the process is even spawned, so there's no gap in which a
     * caller that polls audio__file_is_open() immediately after this
     * function returns (ffplay's wait loop, for one) could see "not open"
     * and mistake "hasn't opened yet" for "already finished" -- see
     * s_file_opening's own comment above. Cleared once the spawned process
     * actually opens or fails (audio__file_publish_open_result(), the two
     * success paths, or, if it's force-killed before either, the process's
     * own cleanup callback). */
    s_file_opening = true;
    xSemaphoreGive(s_audio_mutex);
    context->token = token;

    process_create_params_t params = {
        .name = "audio.file",
        .built_in = true,
        .start_in_background = true,
        .stack_bytes = AUDIO__FILE_PLAY_TASK_STACK_BYTES,
        .priority = AUDIO__PRODUCER_TASK_PRIORITY,
        .process_entry = is_wav ? audio__file_play_process_entry : audio__mp3_play_process_entry,
        .process_entry_context = context,
        .process_entry_cleanup = audio__file_play_process_cleanup,
    };
    bruce_process_id_t spawned_id = BRUCE_PROCESS_ID_INVALID;
    bruce_result_t created = process_registry__create(&params, &spawned_id);
    if (created != BRUCE_OK) {
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        s_file_opening = false;
        xSemaphoreGive(s_audio_mutex);
        free(context->path);
        free(context);
        return created;
    }
    return BRUCE_OK;
}

bruce_result_t audio__file_pause(void) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    bruce_result_t result = s_file_open ? BRUCE_OK : BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK) s_file_playing = false;
    xSemaphoreGive(s_audio_mutex);
    return result;
}

bruce_result_t audio__file_resume(void) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    bruce_result_t result = s_file_open ? BRUCE_OK : BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK) s_file_playing = true;
    xSemaphoreGive(s_audio_mutex);
    return result;
}

bruce_result_t audio__file_seek(uint32_t position_ms) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    bruce_result_t result = s_file_open ? BRUCE_OK : BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK && s_file_format == AUDIO__FILE_FORMAT_MP3) {
        result = BRUCE_ERR_UNSUPPORTED;
    } else if (result == BRUCE_OK) {
        s_file_seek_target_ms = position_ms;
        s_file_seek_requested = true;
    }
    xSemaphoreGive(s_audio_mutex);
    return result;
}

bruce_result_t audio__file_stop(void) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    bruce_result_t result = s_file_open ? BRUCE_OK : BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK) s_file_stop_requested = true;
    xSemaphoreGive(s_audio_mutex);
    return result;
}

bool audio__file_is_open(void) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    bool open = s_file_open || s_file_opening;
    xSemaphoreGive(s_audio_mutex);
    return open;
}

bool audio__file_is_playing(void) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    bool playing = s_file_open && s_file_playing;
    xSemaphoreGive(s_audio_mutex);
    return playing;
}

uint32_t audio__file_position_ms(void) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    uint32_t ms = 0;
    if (s_file_open && s_file_wav.sample_rate > 0) {
        uint32_t frame_bytes = (uint32_t)s_file_wav.channels * ((uint32_t)s_file_wav.bits_per_sample / 8u);
        uint32_t frames = s_file_wav.data_position / frame_bytes;
        ms = (uint32_t)(((uint64_t)frames * 1000u) / s_file_wav.sample_rate);
    }
    xSemaphoreGive(s_audio_mutex);
    return ms;
}

uint32_t audio__file_duration_ms(void) {
    audio__ensure_mutex();
    xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    uint32_t ms = 0;
    if (s_file_open && s_file_wav.sample_rate > 0) {
        uint32_t frame_bytes = (uint32_t)s_file_wav.channels * ((uint32_t)s_file_wav.bits_per_sample / 8u);
        uint32_t frames = s_file_wav.data_size / frame_bytes;
        ms = (uint32_t)(((uint64_t)frames * 1000u) / s_file_wav.sample_rate);
    }
    xSemaphoreGive(s_audio_mutex);
    return ms;
}
