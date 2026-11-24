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
static uint64_t s_held_at; /* Last time something kept the device awake. */

/* Dims after the dim time without input, and suspends after the sleep
 * time unless something holds the device awake; then it tries again a
 * sleep time later. */
void device__idle_poll(void) {
    uint64_t now = runtime__now();
    if (s_started_at == 0) s_started_at = s_held_at = now;
    if (now - s_config_at >= DEVICE__IDLE_CONFIG_MS) {
        s_config_at = now;
        s_dim_ms = (uint64_t)config__get_display_dim_timeout() * 1000;
        s_sleep_ms = (uint64_t)config__get_display_sleep_timeout() * 1000;
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
    if (s_sleep_ms == 0 || now - since < s_sleep_ms) return;
    s_held_at = now;
    const char *reason = device__awake_reason();
    if (reason != NULL) return;
    ESP_LOGI(TAG, "idle for %d s: suspending", (int)(s_sleep_ms / 1000));
    if (app_runner__run_command("suspend", BRUCE_LAUNCH_BACKGROUND) < 0) ESP_LOGW(TAG, "suspend failed to start");
}
