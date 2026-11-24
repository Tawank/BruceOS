#include "core_sdk/haptic.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "hal/hal_haptic.h"

#define HAPTIC__RELEASE_MS 2000 /* Powered this long after the last effect. */

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_release;
static bool s_held;

static void haptic__lock(void) {
    if (s_lock == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void haptic__unlock(void) { xSemaphoreGive(s_lock); }

static void haptic__release(void *context) {
    const hal_device_t *motor = context;
    haptic__lock();
    if (s_held) hal_device_put(motor);
    s_held = false;
    haptic__unlock();
}

bool haptic__is_available(void) { return hal_device_first(HAL_CLASS_HAPTIC) != NULL; }

bruce_result_t haptic__play(bruce_haptic_effect_t effect) {
    const hal_device_t *motor = hal_device_first(HAL_CLASS_HAPTIC);
    if (motor == NULL) return BRUCE_ERR_UNSUPPORTED;
    if ((unsigned)effect > BRUCE_HAPTIC_ALERT) return BRUCE_ERR_INVALID_ARGUMENT;
    haptic__lock();
    bruce_result_t result = BRUCE_OK;
    if (s_release == NULL) {
        const esp_timer_create_args_t args = {.callback = haptic__release, .arg = (void *)motor, .name = "haptic"};
        if (esp_timer_create(&args, &s_release) != ESP_OK) result = BRUCE_ERR_NO_MEMORY;
    }
    if (result == BRUCE_OK && !s_held) {
        result = hal_device_get(motor);
        s_held = result == BRUCE_OK;
    }
    if (result == BRUCE_OK) result = hal_haptic_play(motor, (hal_haptic_effect_t)effect);
    if (s_held) {
        (void)esp_timer_stop(s_release);
        (void)esp_timer_start_once(s_release, HAPTIC__RELEASE_MS * 1000ULL);
    }
    haptic__unlock();
    return result;
}
