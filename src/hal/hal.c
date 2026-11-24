#include "hal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "hal_rail.h"

static const char *TAG = "hal";

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
/* Board indexes in the order they became ready, for suspending in reverse. */
static uint16_t *s_order;
static size_t s_order_count;

static void hal__lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void hal__unlock(void) { xSemaphoreGiveRecursive(s_lock); }

const char *hal__state_name(hal_state_t state) {
    switch (state) {
        case HAL_STATE_PENDING: return "pending";
        case HAL_STATE_READY: return "ready";
        case HAL_STATE_FAILED: return "failed";
        case HAL_STATE_ABSENT: return "absent";
        case HAL_STATE_SUSPENDED: return "suspended";
    }
    return "?";
}

/* HAL_STATE_READY when everything the device needs is ready, PENDING while
 * something is still pending, FAILED when something will never be. */
static hal_state_t hal__dependency(const hal_device_t *needed) {
    if (needed == NULL) return HAL_STATE_READY;
    hal_state_t state = needed->state->state;
    return state == HAL_STATE_READY || state == HAL_STATE_PENDING ? state : HAL_STATE_FAILED;
}

static hal_state_t hal__dependencies(const hal_device_t *device) {
    hal_state_t worst = hal__dependency(device->bus);
    for (size_t i = 0; i < device->pin_count && worst != HAL_STATE_FAILED; ++i) {
        hal_state_t state = hal__dependency(device->pins[i].pin.controller);
        if (state != HAL_STATE_READY) worst = state;
    }
    for (size_t i = 0; i < device->rail_count && worst != HAL_STATE_FAILED; ++i) {
        hal_state_t state = hal__dependency(device->rails[i]);
        if (state != HAL_STATE_READY) worst = state;
    }
    return worst;
}

static void hal__settle(const hal_device_t *device, hal_state_t state, bruce_result_t error) {
    device->state->state = state;
    device->state->error = error;
    if (state == HAL_STATE_FAILED) ESP_LOGW(TAG, "%s: failed (%d)", device->name, (int)error);
}

/* One attempt; returns true when the device left HAL_STATE_PENDING. */
static bool hal__probe(const hal_device_t *device) {
    hal_state_t needs = hal__dependencies(device);
    if (needs == HAL_STATE_PENDING) return false;
    if (needs == HAL_STATE_FAILED) {
        hal__settle(device, device->optional ? HAL_STATE_ABSENT : HAL_STATE_FAILED, BRUCE_ERR_NOT_FOUND);
        return true;
    }
    bruce_result_t result = device->driver != NULL && device->driver->probe != NULL ? device->driver->probe(device)
                                                                                    : BRUCE_OK;
    if (result == BRUCE_ERR_BUSY) return false;
    if (result == BRUCE_OK) hal__settle(device, HAL_STATE_READY, BRUCE_OK);
    else if (device->optional && result == BRUCE_ERR_NOT_FOUND) hal__settle(device, HAL_STATE_ABSENT, result);
    else hal__settle(device, HAL_STATE_FAILED, result);
    return true;
}

/* Passes over the board until nothing changes; what is left waits on itself. */
void hal__init(void) {
    if (s_lock != NULL) return;
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
    s_order = calloc(hal_board.device_count > 0 ? hal_board.device_count : 1, sizeof(*s_order));
    hal__lock();
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < hal_board.device_count; ++i) {
            const hal_device_t *device = hal_board.devices[i];
            if (device->state->state != HAL_STATE_PENDING || !hal__probe(device)) continue;
            progress = true;
            if (device->state->state == HAL_STATE_READY && s_order != NULL) s_order[s_order_count++] = (uint16_t)i;
        }
    }
    for (size_t i = 0; i < hal_board.device_count; ++i) {
        const hal_device_t *device = hal_board.devices[i];
        if (device->state->state == HAL_STATE_PENDING) hal__settle(device, HAL_STATE_FAILED, BRUCE_ERR_INVALID_STATE);
    }
    hal__unlock();
    ESP_LOGI(TAG, "%s: %u devices, %u ready", hal_board.name, (unsigned)hal_board.device_count,
             (unsigned)s_order_count);
}

const hal_device_t *hal_device_find(const char *name) {
    for (size_t i = 0; i < hal_board.device_count; ++i) {
        if (strcmp(hal_board.devices[i]->name, name) == 0) return hal_board.devices[i];
    }
    return NULL;
}

const hal_device_t *hal_device_first(hal_class_t class_id) {
    for (size_t i = 0; i < hal_board.device_count; ++i) {
        const hal_device_t *device = hal_board.devices[i];
        hal_class_t device_class = device->driver != NULL ? device->driver->class_id : HAL_CLASS_NONE;
        if (device_class == class_id && device->state->state == HAL_STATE_READY) return device;
    }
    return NULL;
}

bool hal_device_ready(const hal_device_t *device) {
    return device != NULL && device->state->state == HAL_STATE_READY;
}

const hal_pin_t *hal_device_pin(const hal_device_t *device, const char *role) {
    if (device == NULL) return NULL;
    for (size_t i = 0; i < device->pin_count; ++i) {
        if (strcmp(device->pins[i].role, role) == 0) return &device->pins[i].pin;
    }
    return NULL;
}

bruce_result_t hal_device_power(const hal_device_t *device, bool on) {
    if (device == NULL || device->rail_count == 0) return BRUCE_OK;
    if (s_lock == NULL) return BRUCE_ERR_INVALID_STATE;
    hal__lock();
    bruce_result_t result = BRUCE_OK;
    if (device->state->powered != on) {
        for (size_t i = 0; i < device->rail_count; ++i) {
            bruce_result_t rail = hal_rail__use(device->rails[i], on);
            if (rail != BRUCE_OK) result = rail;
        }
        device->state->powered = on;
    }
    hal__unlock();
    return result;
}

bruce_result_t hal_device_get(const hal_device_t *device) {
    if (!hal_device_ready(device)) return BRUCE_ERR_NOT_FOUND;
    if (device->driver == &hal_rail_driver) return BRUCE_ERR_INVALID_ARGUMENT;
    hal__lock();
    bruce_result_t result = BRUCE_OK;
    if (device->state->users == 0 && device->driver->start != NULL) result = device->driver->start(device);
    if (result == BRUCE_OK) device->state->users++;
    hal__unlock();
    return result;
}

void hal_device_put(const hal_device_t *device) {
    if (device == NULL || device->driver == NULL || device->driver == &hal_rail_driver) return;
    hal__lock();
    if (device->state->users > 0 && --device->state->users == 0 && device->driver->stop != NULL) {
        device->driver->stop(device);
    }
    hal__unlock();
}

static void hal__change(bool suspend) {
    if (s_lock == NULL) return;
    hal__lock();
    for (size_t n = 0; n < s_order_count; ++n) {
        const hal_device_t *device = hal_board.devices[s_order[suspend ? s_order_count - 1 - n : n]];
        const hal_driver_t *driver = device->driver;
        hal_state_t from = suspend ? HAL_STATE_READY : HAL_STATE_SUSPENDED;
        if (device->state->state != from) continue;
        bruce_result_t (*step)(const hal_device_t *) =
            driver == NULL ? NULL : suspend ? driver->suspend : driver->resume;
        if (step != NULL && step(device) != BRUCE_OK) {
            ESP_LOGW(TAG, "%s: %s failed", device->name, suspend ? "suspend" : "resume");
            continue;
        }
        device->state->state = suspend ? HAL_STATE_SUSPENDED : HAL_STATE_READY;
    }
    hal__unlock();
}

void hal__suspend(void) { hal__change(true); }
void hal__resume(void) { hal__change(false); }

void hal__for_each(void (*visit)(const hal_device_t *device, void *context), void *context) {
    for (size_t i = 0; i < hal_board.device_count; ++i) visit(hal_board.devices[i], context);
}
