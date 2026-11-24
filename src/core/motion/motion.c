#include "core_sdk/motion.h"

#include "core_sdk/permission.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_imu.h"

#define MOTION__POLL_MS 40      /* The sensor reports at 25 Hz. */
#define MOTION__IDLE_MS 5000    /* Off this long after the last read. */
#define MOTION__RETRY_MS 30000  /* After a failed start (e.g. no firmware file). */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_on;
static bool s_ready;
static float s_yaw;
static uint64_t s_read_at;
static uint64_t s_failed_at;

static uint64_t motion__now(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static bool motion__wanted(void) {
    portENTER_CRITICAL(&s_mux);
    bool wanted = motion__now() - s_read_at < MOTION__IDLE_MS;
    portEXIT_CRITICAL(&s_mux);
    return wanted;
}

/* Starts the IMU when woken, reads it while someone asks, then lets it go. */
static void motion__task(void *context) {
    const hal_device_t *imu = context;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool started = hal_device_get(imu) == BRUCE_OK;
        while (started && motion__wanted()) {
            hal_imu_reading_t reading;
            if (hal_imu_read(imu, &reading) == BRUCE_OK && reading.valid) {
                portENTER_CRITICAL(&s_mux);
                s_ready = true;
                s_yaw = reading.yaw_deg;
                portEXIT_CRITICAL(&s_mux);
            }
            vTaskDelay(pdMS_TO_TICKS(MOTION__POLL_MS));
        }
        if (started) hal_device_put(imu);
        portENTER_CRITICAL(&s_mux);
        if (!started) s_failed_at = motion__now();
        s_on = s_ready = false;
        portEXIT_CRITICAL(&s_mux);
    }
}

/* Created by the first reader; the others wait for it. */
static bool motion__start_task(const hal_device_t *imu) {
    static bool s_creating;
    portENTER_CRITICAL(&s_mux);
    bool create = !s_creating;
    s_creating = true;
    portEXIT_CRITICAL(&s_mux);
    /* Its stack holds the firmware upload's file reads. */
    if (create && xTaskCreate(motion__task, "motion", 3072, (void *)imu, 5, &s_task) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_creating = false;
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    while (s_task == NULL) vTaskDelay(1);
    return true;
}

bool motion__is_available(void) { return hal_device_first(HAL_CLASS_IMU) != NULL; }

bruce_result_t motion__read(bruce_motion_t *out) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPS);
    if (permission != BRUCE_OK) return permission;
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out = (bruce_motion_t){0};
    const hal_device_t *imu = hal_device_first(HAL_CLASS_IMU);
    if (imu == NULL) return BRUCE_ERR_UNSUPPORTED;
    if (!motion__start_task(imu)) return BRUCE_ERR_NO_MEMORY;
    uint64_t now = motion__now();
    portENTER_CRITICAL(&s_mux);
    s_read_at = now;
    bool wake = !s_on && (s_failed_at == 0 || now - s_failed_at >= MOTION__RETRY_MS);
    if (wake) s_on = true;
    out->ready = s_ready;
    out->yaw_deg = s_yaw;
    portEXIT_CRITICAL(&s_mux);
    if (wake) xTaskNotifyGive(s_task);
    return BRUCE_OK;
}
