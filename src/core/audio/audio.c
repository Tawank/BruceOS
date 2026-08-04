#include "core_sdk/audio.h"

#include <stdlib.h>

#include "core/config/config.h"
#include "core/process/process.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BRUCE_AUDIO_BACKEND_I2S
#include "driver/i2s_std.h"
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

static void audio__ensure_mutex(void) {
    if (s_audio_mutex != NULL) return;
    portENTER_CRITICAL(&s_audio_init_mux);
    if (s_audio_mutex == NULL) { s_audio_mutex = xSemaphoreCreateMutexStatic(&s_audio_mutex_storage); }
    portEXIT_CRITICAL(&s_audio_init_mux);
}

#if CONFIG_BRUCE_AUDIO_BACKEND_I2S && !CONFIG_BRUCE_QEMU_TEST_MODE
static bruce_result_t audio__play_i2s(const audio__tone_params_t *params) {
    i2s_chan_handle_t tx_channel = NULL;
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&channel_config, &tx_channel, NULL) != ESP_OK) return BRUCE_ERR_BUSY;

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
    if (i2s_channel_init_std_mode(tx_channel, &config) != ESP_OK || i2s_channel_enable(tx_channel) != ESP_OK) {
        i2s_del_channel(tx_channel);
        return BRUCE_ERR_INTERNAL;
    }

    int16_t samples[AUDIO__I2S_BUFFER_FRAMES * 2u];
    int16_t amplitude = (int16_t)((INT16_MAX * params->volume) / 100u);
    uint32_t phase = 0;
    uint32_t frames_remaining = (AUDIO__I2S_SAMPLE_RATE * params->duration_ms) / 1000u;
    bruce_result_t result = BRUCE_OK;
    while (frames_remaining > 0) {
        uint32_t frames = frames_remaining < AUDIO__I2S_BUFFER_FRAMES ? frames_remaining : AUDIO__I2S_BUFFER_FRAMES;
        for (uint32_t i = 0; i < frames; ++i) {
            int16_t sample = phase < AUDIO__I2S_SAMPLE_RATE / 2u ? amplitude : (int16_t)-amplitude;
            samples[i * 2u] = sample;
            samples[i * 2u + 1u] = sample;
            phase += params->frequency_hz;
            phase %= AUDIO__I2S_SAMPLE_RATE;
        }
        size_t bytes_written = 0;
        size_t bytes = frames * 2u * sizeof(samples[0]);
        if (i2s_channel_write(tx_channel, samples, bytes, &bytes_written, portMAX_DELAY) != ESP_OK ||
            bytes_written != bytes) {
            result = BRUCE_ERR_IO;
            break;
        }
        frames_remaining -= frames;
    }

    vTaskDelay(pdMS_TO_TICKS(6));
    i2s_channel_disable(tx_channel);
    i2s_del_channel(tx_channel);
    return result;
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
