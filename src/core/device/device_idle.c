#include "core/audio/audio_power.h"
#include "core/cellular/cellular.h"
#include "core/device/device.h"
#include "core/display/display.h"
#include "core/event_loop/event_loop.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/runtime.h"

#include "esp_log.h"

#define TAG "device"
#define DEVICE__IDLE_CONFIG_MS 1000

static uint64_t s_started_at;
static uint64_t s_config_at;
static uint64_t s_dim_ms;
static uint64_t s_sleep_ms;
static bool s_dimmed;
static uint64_t s_held_at; /* Last time something kept the device from sleeping. */

/* With the screen off: Select or a call turns it back on, and after idle
 * sleep any input. */
static void device__idle_asleep(void) {
    bool select = device__pm_select_pressed();
    if (select || device__pm_input_since_sleep()) {
        device__pm_wake(select);
    } else if (cellular__in_call()) {
        device__pm_wake(false);
    } else {
        device__pm_hold(device__awake_reason() != NULL || device__chip_reason() != NULL);
    }
}

/* Dims after the dim time without input, and sleeps after the sleep time
 * unless something holds the device awake; then it tries again a sleep
 * time later. Once dimmed, the chip may light-sleep when nothing needs it. */
void device__idle_poll(void) {
    uint64_t now = runtime__now();
    if (s_started_at == 0) s_started_at = s_held_at = now;
    if (now - s_config_at >= DEVICE__IDLE_CONFIG_MS) {
        s_config_at = now;
        s_dim_ms = (uint64_t)config__get_display_dim_timeout() * 1000;
        s_sleep_ms = (uint64_t)config__get_display_sleep_timeout() * 1000;
        audio__power_idle();
    }
    if (device__pm_asleep()) {
        device__idle_asleep();
        return;
    }
    if (cellular__in_call()) event_loop__note_input(); /* Ringing or talking: screen up. */
    uint64_t last_input = event_loop__last_input_ms();
    uint64_t active = last_input > s_started_at ? last_input : s_started_at;

    bool dim = s_dim_ms > 0 && now - active >= s_dim_ms;
    if (dim != s_dimmed) {
        s_dimmed = dim;
        display__dim(dim);
        device__keyboard_light_apply(!dim);
    }

    uint64_t since = active > s_held_at ? active : s_held_at;
    bool sleep_due = s_sleep_ms != 0 && now - since >= s_sleep_ms;
    /* Light sleep only once dimmed: touch pulses are too short to wake it. */
    bool bright = !dim;
    const char *reason = bright && !sleep_due ? NULL : device__awake_reason();
    device__pm_hold(bright || reason != NULL || device__chip_reason() != NULL);

    if (!sleep_due) return;
    s_held_at = now;
    if (reason != NULL || device__app_awake()) return;
    ESP_LOGI(TAG, "idle for %d s: suspending", (int)(s_sleep_ms / 1000));
    device__pm_next_any_input(true);
    if (app_runner__run_command("suspend", BRUCE_LAUNCH_BACKGROUND) < 0) {
        device__pm_next_any_input(false);
        ESP_LOGW(TAG, "suspend failed to start");
    }
}
