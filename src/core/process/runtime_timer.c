#include "process_internal.h"

#include "core_sdk/runtime.h"

#include <stdlib.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

typedef struct runtime__timer {
    bruce_process_id_t owner;
    bruce_timer_id_t id;
    volatile uint32_t *counter;
    esp_timer_handle_t handle;
    SemaphoreHandle_t ticks;
    struct runtime__timer *next;
} runtime__timer_t;

static runtime__timer_t *s_timers;
static portMUX_TYPE s_timers_lock = portMUX_INITIALIZER_UNLOCKED;

static void runtime__timer_alarm(void *argument) {
    runtime__timer_t *timer = argument;
    taskENTER_CRITICAL(&s_timers_lock);
    if (timer->counter != NULL) {
        __atomic_fetch_add(timer->counter, 1u, __ATOMIC_RELAXED);
        (void)xSemaphoreGive(timer->ticks);
    }
    taskEXIT_CRITICAL(&s_timers_lock);
}

static void runtime__timer_cleanup(void *context) {
    runtime__timer_t *timer = context;
    if (timer == NULL) return;

    taskENTER_CRITICAL(&s_timers_lock);
    /* An alarm already holding this lock completes its increment before the
     * owner can release the counter's storage. */
    timer->counter = NULL;
    runtime__timer_t **link = &s_timers;
    while (*link != NULL && *link != timer) link = &(*link)->next;
    if (*link == timer) *link = timer->next;
    taskEXIT_CRITICAL(&s_timers_lock);

    (void)esp_timer_stop(timer->handle);
    (void)esp_timer_delete(timer->handle);
    vSemaphoreDelete(timer->ticks);
    free(timer);
}

bruce_result_t runtime__timer_start(
    uint32_t period_us, volatile uint32_t *counter, bruce_timer_id_t *out_timer_id
) {
    if (period_us == 0 || counter == NULL || out_timer_id == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_timer_id = BRUCE_TIMER_ID_INVALID;

    bruce_process_id_t owner = process__current_id();
    if (owner == BRUCE_PROCESS_ID_INVALID) return BRUCE_ERR_INVALID_STATE;

    runtime__timer_t *timer = calloc(1, sizeof(*timer));
    if (timer == NULL) return BRUCE_ERR_NO_MEMORY;
    timer->owner = owner;
    timer->counter = counter;
    timer->ticks = xSemaphoreCreateBinary();
    if (timer->ticks == NULL) {
        free(timer);
        return BRUCE_ERR_NO_MEMORY;
    }

    esp_timer_create_args_t arguments = {
        .callback = runtime__timer_alarm,
        .arg = timer,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bruce_periodic",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&arguments, &timer->handle) != ESP_OK) {
        vSemaphoreDelete(timer->ticks);
        free(timer);
        return BRUCE_ERR_NO_MEMORY;
    }

    timer->id = process_registry__resource_register(runtime__timer_cleanup, timer);
    if (timer->id == BRUCE_TIMER_ID_INVALID) {
        (void)esp_timer_delete(timer->handle);
        vSemaphoreDelete(timer->ticks);
        free(timer);
        return BRUCE_ERR_NO_MEMORY;
    }

    taskENTER_CRITICAL(&s_timers_lock);
    timer->next = s_timers;
    s_timers = timer;
    taskEXIT_CRITICAL(&s_timers_lock);

    if (esp_timer_start_periodic(timer->handle, period_us) != ESP_OK) {
        bruce_timer_id_t id = timer->id;
        runtime__timer_cleanup(timer);
        (void)process_registry__resource_release(id);
        return BRUCE_ERR_IO;
    }

    *out_timer_id = timer->id;
    return BRUCE_OK;
}

bruce_result_t runtime__timer_wait(bruce_timer_id_t timer_id, uint32_t timeout_ms) {
    if (timer_id == BRUCE_TIMER_ID_INVALID) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_id_t owner = process__current_id();
    if (owner == BRUCE_PROCESS_ID_INVALID) return BRUCE_ERR_INVALID_STATE;

    taskENTER_CRITICAL(&s_timers_lock);
    runtime__timer_t *timer = s_timers;
    while (timer != NULL && (timer->owner != owner || timer->id != timer_id)) timer = timer->next;
    SemaphoreHandle_t ticks = timer != NULL ? timer->ticks : NULL;
    taskEXIT_CRITICAL(&s_timers_lock);
    if (ticks == NULL) return BRUCE_ERR_NOT_FOUND;

    TickType_t timeout = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(ticks, timeout) == pdTRUE ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
}

bruce_result_t runtime__timer_stop(bruce_timer_id_t timer_id) {
    if (timer_id == BRUCE_TIMER_ID_INVALID) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_id_t owner = process__current_id();
    if (owner == BRUCE_PROCESS_ID_INVALID) return BRUCE_ERR_INVALID_STATE;

    taskENTER_CRITICAL(&s_timers_lock);
    runtime__timer_t *timer = s_timers;
    while (timer != NULL && (timer->owner != owner || timer->id != timer_id)) timer = timer->next;
    taskEXIT_CRITICAL(&s_timers_lock);
    if (timer == NULL) return BRUCE_ERR_NOT_FOUND;

    bruce_result_t result = process_registry__resource_release(timer_id);
    if (result != BRUCE_OK) return result;
    runtime__timer_cleanup(timer);
    return BRUCE_OK;
}
