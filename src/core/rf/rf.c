#include "core_sdk/rf.h"

#include <string.h>

#include "core/process/process.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "hal/hal_rf.h"

#define RF__MODULES_MAX 4

typedef struct {
    const hal_device_t *device;
    bruce_process_id_t owner;
    bruce_resource_id_t resource;
} rf__module_t;

static rf__module_t s_modules[RF__MODULES_MAX];
static size_t s_count;
static bool s_found;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;

static void rf__add(const hal_device_t *device, void *context) {
    (void)context;
    if (s_count < RF__MODULES_MAX && hal_device_ready(device) && device->driver != NULL &&
        device->driver->class_id == HAL_CLASS_RF) {
        s_modules[s_count++] = (rf__module_t){.device = device, .owner = BRUCE_PROCESS_ID_INVALID};
    }
}

/* The board's RF devices, found once (the HAL probes them at boot). */
static void rf__lock(void) {
    static portMUX_TYPE spin = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&spin);
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    taskEXIT_CRITICAL(&spin);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_found) {
        hal__for_each(rf__add, NULL);
        s_found = true;
    }
}

static void rf__unlock(void) { xSemaphoreGive(s_lock); }

static void rf__release_locked(rf__module_t *module) {
    module->owner = BRUCE_PROCESS_ID_INVALID;
    module->resource = BRUCE_RESOURCE_ID_INVALID;
    hal_device_put(module->device);
}

/* The holder exited without rf__close(). */
static void rf__cleanup(void *context) {
    rf__lock();
    rf__module_t *module = context;
    if (module->owner != BRUCE_PROCESS_ID_INVALID) rf__release_locked(module);
    rf__unlock();
}

size_t rf__count(void) {
    rf__lock();
    size_t count = s_count;
    rf__unlock();
    return count;
}

bruce_result_t rf__info(size_t index, bruce_rf_module_t *out_module) {
    if (out_module == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    rf__lock();
    if (index >= s_count) {
        rf__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    const rf__module_t *module = &s_modules[index];
    memset(out_module, 0, sizeof(*out_module));
    strlcpy(out_module->name, module->device->name, sizeof(out_module->name));
    strlcpy(out_module->chip, module->device->driver->name, sizeof(out_module->chip));
    const hal_rf_band_t *bands = NULL;
    size_t count = hal_rf_bands(module->device, &bands);
    for (size_t i = 0; i < count && i < BRUCE_RF_BANDS_MAX; ++i) {
        out_module->bands[i] = (bruce_rf_band_t){bands[i].min_khz, bands[i].max_khz};
        out_module->band_count++;
    }
    out_module->in_use = module->owner != BRUCE_PROCESS_ID_INVALID && module->owner != process__current_id();
    rf__unlock();
    return BRUCE_OK;
}

bruce_result_t rf__open(size_t index) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_RF);
    if (result != BRUCE_OK) return result;
    bruce_process_id_t caller = process__current_id();
    rf__lock();
    if (index >= s_count) {
        rf__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    rf__module_t *module = &s_modules[index];
    if (module->owner == caller) {
        rf__unlock();
        return BRUCE_OK;
    }
    if (module->owner != BRUCE_PROCESS_ID_INVALID) {
        rf__unlock();
        return BRUCE_ERR_BUSY;
    }
    result = hal_device_get(module->device);
    if (result == BRUCE_OK) module->owner = caller;
    rf__unlock();
    if (result != BRUCE_OK) return result;
    /* Outside the lock: process teardown runs rf__cleanup() under the registry's. */
    bruce_resource_id_t resource = process_registry__resource_register(rf__cleanup, module);
    rf__lock();
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        rf__release_locked(module);
        result = BRUCE_ERR_RESOURCE_LIMIT;
    } else {
        module->resource = resource;
    }
    rf__unlock();
    return result;
}

bruce_result_t rf__close(size_t index) {
    rf__lock();
    if (index >= s_count || s_modules[index].owner != process__current_id()) {
        rf__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    rf__module_t *module = &s_modules[index];
    bruce_resource_id_t resource = module->resource;
    rf__release_locked(module);
    rf__unlock();
    (void)process_registry__resource_release(resource);
    return BRUCE_OK;
}

bruce_result_t rf__rssi(size_t index, uint32_t frequency_khz, int16_t *out_dbm) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_RF);
    if (result != BRUCE_OK) return result;
    rf__lock();
    if (index >= s_count || s_modules[index].owner != process__current_id()) {
        rf__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    result = hal_rf_rssi(s_modules[index].device, frequency_khz, out_dbm);
    rf__unlock();
    return result;
}
