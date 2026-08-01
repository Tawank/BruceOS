#include "event_loop.h"

#include "core/process/process.h"
#include "core_sdk/input.h"
#include "core_sdk/permission.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define EVENT_LOOP__QUEUE_LENGTH 16

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static StaticQueue_t s_queue_storage;
static uint8_t s_queue_buffer[EVENT_LOOP__QUEUE_LENGTH * sizeof(bruce_input_event_t)];
static QueueHandle_t s_queue;
static bool s_initialized;
static bruce_process_id_t s_foreground_process_id;
static uint32_t s_foreground_epoch;

static void event_loop__lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }

static void event_loop__unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static uint64_t event_loop__now_ms(void) {
    return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static bool event_loop__caller_is_foreground_locked(bruce_process_id_t caller) {
    return caller != BRUCE_PROCESS_ID_INVALID && caller == s_foreground_process_id;
}

bruce_result_t event_loop__init(void) {
    if (s_mutex == NULL) { s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutex_storage); }
    if (s_mutex == NULL) { return BRUCE_ERR_NO_MEMORY; }

    event_loop__lock();
    if (s_initialized) {
        event_loop__unlock();
        return BRUCE_OK;
    }

    s_queue = xQueueCreateStatic(
        EVENT_LOOP__QUEUE_LENGTH, sizeof(bruce_input_event_t), s_queue_buffer, &s_queue_storage
    );
    if (s_queue == NULL) {
        event_loop__unlock();
        return BRUCE_ERR_INTERNAL;
    }

    s_initialized = true;
    event_loop__unlock();
    return BRUCE_OK;
}

void event_loop__deinit(void) {
    if (s_mutex == NULL) { return; }
    event_loop__lock();
    if (!s_initialized) {
        event_loop__unlock();
        return;
    }

    s_initialized = false;
    bruce_process_id_t owner = s_foreground_process_id;
    s_foreground_process_id = BRUCE_PROCESS_ID_INVALID;
    s_foreground_epoch++;
    event_loop__unlock();
    process_registry__event_wake(owner);
}

void event_loop__foreground_changed(bruce_process_id_t process_id) {
    if (s_mutex == NULL) {
        s_foreground_process_id = process_id;
        s_foreground_epoch++;
        return;
    }
    event_loop__lock();
    if (s_foreground_process_id != process_id) {
        s_foreground_process_id = process_id;
        s_foreground_epoch++;
    }
    event_loop__unlock();
}

bruce_result_t input__read(bruce_input_event_t *out_event, uint32_t timeout_ms) {
    if (out_event == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_process_id_t caller = process__current_id();
    uint64_t start_ms = event_loop__now_ms();
    event_loop__lock();
    if (!s_initialized) {
        event_loop__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (!event_loop__caller_is_foreground_locked(caller)) {
        event_loop__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    uint32_t epoch = s_foreground_epoch;
    event_loop__unlock();

    for (;;) {
        if (process_registry__event_wake_clear(caller) != BRUCE_OK) { return BRUCE_ERR_NOT_FOREGROUND; }
        event_loop__lock();
        if (!s_initialized) {
            event_loop__unlock();
            return BRUCE_ERR_NOT_INITIALIZED;
        }
        if (!event_loop__caller_is_foreground_locked(caller) || s_foreground_epoch != epoch) {
            event_loop__unlock();
            return BRUCE_ERR_NOT_FOREGROUND;
        }
        BaseType_t received = xQueueReceive(s_queue, out_event, 0);
        event_loop__unlock();
        if (received == pdPASS) { return BRUCE_OK; }
        if (timeout_ms == 0) { return BRUCE_ERR_TIMEOUT; }
        uint32_t remaining = portMAX_DELAY;
        if (timeout_ms != portMAX_DELAY) {
            uint64_t elapsed = event_loop__now_ms() - start_ms;
            if (elapsed >= timeout_ms) { return BRUCE_ERR_TIMEOUT; }
            remaining = (uint32_t)(timeout_ms - elapsed);
        }
        (void)process_registry__event_wake_wait(caller, remaining);
    }
}

bruce_result_t input__flush(void) {
    bruce_process_id_t caller = process__current_id();
    event_loop__lock();
    if (!s_initialized) {
        event_loop__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (!event_loop__caller_is_foreground_locked(caller)) {
        event_loop__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    bruce_input_event_t event;
    while (xQueueReceive(s_queue, &event, 0) == pdPASS) {}
    event_loop__unlock();
    return BRUCE_OK;
}

bruce_result_t input__peek(bruce_input_event_t *out_event) {
    if (out_event == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_process_id_t caller = process__current_id();
    event_loop__lock();
    if (!s_initialized) {
        event_loop__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    if (!event_loop__caller_is_foreground_locked(caller)) {
        event_loop__unlock();
        return BRUCE_ERR_NOT_FOREGROUND;
    }
    BaseType_t received = xQueuePeek(s_queue, out_event, 0);
    event_loop__unlock();
    return received == pdPASS ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
}

bruce_result_t input__wait(uint32_t timeout_ms, int32_t *out_code) {
    if (out_code == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    uint64_t start_ms = event_loop__now_ms();
    for (;;) {
        uint32_t remaining;
        if (timeout_ms == portMAX_DELAY) {
            remaining = portMAX_DELAY;
        } else {
            uint64_t elapsed_ms = event_loop__now_ms() - start_ms;
            remaining = elapsed_ms >= timeout_ms ? 0 : (uint32_t)(timeout_ms - elapsed_ms);
        }

        bruce_input_event_t event;
        bruce_result_t result = input__read(&event, remaining);
        if (result != BRUCE_OK) { return result; }
        if (event.action == BRUCE_INPUT_PRESS) {
            *out_code = event.code;
            return BRUCE_OK;
        }
    }
}

bool input__check(int32_t code, bool consume) {
    bruce_process_id_t caller = process__current_id();
    event_loop__lock();
    if (!s_initialized || !event_loop__caller_is_foreground_locked(caller)) {
        event_loop__unlock();
        return false;
    }

    UBaseType_t count = uxQueueMessagesWaiting(s_queue);
    if (count == 0 || count > EVENT_LOOP__QUEUE_LENGTH) {
        event_loop__unlock();
        return false;
    }

    bruce_input_event_t events[EVENT_LOOP__QUEUE_LENGTH];
    for (UBaseType_t i = 0; i < count; ++i) { (void)xQueueReceive(s_queue, &events[i], 0); }

    bool found = false;
    UBaseType_t found_index = 0;
    for (UBaseType_t i = 0; i < count; ++i) {
        if (events[i].action == BRUCE_INPUT_PRESS && events[i].code == code) {
            found = true;
            found_index = i;
            break;
        }
    }

    for (UBaseType_t i = count; i-- > 0;) {
        if (found && consume && i == found_index) { continue; }
        (void)xQueueSendToFront(s_queue, &events[i], 0);
    }

    event_loop__unlock();
    return found;
}

bruce_result_t input__inject(const bruce_input_event_t *event) {
    if (event == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    if (event->type != BRUCE_INPUT_KEY && event->type != BRUCE_INPUT_BUTTON &&
        event->type != BRUCE_INPUT_TOUCH && event->type != BRUCE_INPUT_ENCODER &&
        event->type != BRUCE_INPUT_CUSTOM) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (event->action != BRUCE_INPUT_PRESS && event->action != BRUCE_INPUT_RELEASE &&
        event->action != BRUCE_INPUT_CHANGE) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_INPUT);
    if (permission != BRUCE_OK) { return permission; }

    bruce_input_event_t queued_event = *event;
    queued_event.timestamp_ms = event_loop__now_ms();
    queued_event.source_process_id = process__current_id();

    event_loop__lock();
    if (!s_initialized) {
        event_loop__unlock();
        return BRUCE_ERR_NOT_INITIALIZED;
    }
    BaseType_t sent = xQueueSend(s_queue, &queued_event, 0);
    bruce_process_id_t owner = s_foreground_process_id;
    event_loop__unlock();
    process_registry__event_wake(owner);
    return sent == pdPASS ? BRUCE_OK : BRUCE_ERR_BUSY;
}
