#include "spi.h"

#include <string.h>

#include "core/task/task.h"
#include "core_sdk/permission.h"

#include "driver/gpio.h" // IWYU pragma: export
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SPI__HOST SPI3_HOST
#define SPI__MAX_DEVICES 8

typedef struct {
    bool in_use;
    bool task_owned;
    bruce_spi_id_t id;
    bruce_task_id_t owner;
    bruce_resource_id_t resource_id;
    spi_device_handle_t handle;
} spi__slot_t;

static spi__slot_t s_slots[SPI__MAX_DEVICES];
static bruce_spi_id_t s_next_id = 1;
static int s_sck = -1;
static int s_miso = -1;
static int s_mosi = -1;
static size_t s_device_count;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void spi__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void spi__unlock(void) { xSemaphoreGive(s_mutex); }

static bruce_result_t spi__esp_result(esp_err_t error) {
    if (error == ESP_OK) return BRUCE_OK;
    if (error == ESP_ERR_INVALID_ARG) return BRUCE_ERR_INVALID_ARGUMENT;
    if (error == ESP_ERR_NO_MEM) return BRUCE_ERR_NO_MEMORY;
    if (error == ESP_ERR_TIMEOUT) return BRUCE_ERR_TIMEOUT;
    if (error == ESP_ERR_NOT_SUPPORTED) return BRUCE_ERR_UNSUPPORTED;
    if (error == ESP_ERR_INVALID_STATE || error == ESP_ERR_NOT_FOUND) return BRUCE_ERR_BUSY;
    return BRUCE_ERR_IO;
}

static int spi__find_locked(bruce_spi_id_t id) {
    if (id == BRUCE_SPI_ID_INVALID) return -1;
    for (int i = 0; i < SPI__MAX_DEVICES; ++i) {
        if (s_slots[i].in_use && s_slots[i].id == id) return i;
    }
    return -1;
}

static void spi__release_locked(spi__slot_t *slot) {
    (void)spi_bus_remove_device(slot->handle);
    memset(slot, 0, sizeof(*slot));
    if (--s_device_count == 0) {
        (void)spi_bus_free(SPI__HOST);
        s_sck = s_miso = s_mosi = -1;
    }
}

static void spi__cleanup(void *context) {
    spi__lock();
    spi__slot_t *slot = context;
    if (slot->in_use) spi__release_locked(slot);
    spi__unlock();
}

bruce_result_t
spi__open_internal(const bruce_spi_device_config_t *config, bool task_owned, bruce_spi_id_t *out_device) {
    bruce_task_id_t owner = task_owned ? task__current_id() : BRUCE_TASK_ID_INVALID;
    if (out_device != NULL) *out_device = BRUCE_SPI_ID_INVALID;
    if (config == NULL || out_device == NULL || !GPIO_IS_VALID_OUTPUT_GPIO(config->sck) ||
        !GPIO_IS_VALID_GPIO(config->miso) || !GPIO_IS_VALID_OUTPUT_GPIO(config->mosi) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->cs) || config->clock_hz == 0 || config->clock_hz > 80000000u ||
        config->mode > 3) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    spi__lock();
    int index = -1;
    for (int i = 0; i < SPI__MAX_DEVICES; ++i) {
        if (!s_slots[i].in_use) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        spi__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    if (s_device_count > 0 && (config->sck != s_sck || config->miso != s_miso || config->mosi != s_mosi)) {
        spi__unlock();
        return BRUCE_ERR_BUSY;
    }
    if (s_device_count == 0) {
        spi_bus_config_t bus_config = {
            .sclk_io_num = config->sck,
            .miso_io_num = config->miso,
            .mosi_io_num = config->mosi,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = BRUCE_SPI_MAX_TRANSFER_SIZE,
        };
        bruce_result_t result = spi__esp_result(spi_bus_initialize(SPI__HOST, &bus_config, SPI_DMA_DISABLED));
        if (result != BRUCE_OK) {
            spi__unlock();
            return result;
        }
        s_sck = config->sck;
        s_miso = config->miso;
        s_mosi = config->mosi;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = (int)config->clock_hz,
        .mode = config->mode,
        .spics_io_num = config->cs,
        .queue_size = 1,
    };
    spi_device_handle_t handle = NULL;
    bruce_result_t result = spi__esp_result(spi_bus_add_device(SPI__HOST, &device_config, &handle));
    if (result != BRUCE_OK) {
        if (s_device_count == 0) {
            (void)spi_bus_free(SPI__HOST);
            s_sck = s_miso = s_mosi = -1;
        }
        spi__unlock();
        return result;
    }
    s_slots[index] = (spi__slot_t){
        .in_use = true,
        .task_owned = task_owned,
        .handle = handle,
    };
    s_device_count++;
    spi__unlock();

    bruce_resource_id_t resource = BRUCE_RESOURCE_ID_INVALID;
    if (task_owned) {
        resource = task_registry__resource_register(spi__cleanup, &s_slots[index]);
        if (resource == BRUCE_RESOURCE_ID_INVALID) {
            spi__cleanup(&s_slots[index]);
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
    }

    spi__lock();
    bruce_spi_id_t id = s_next_id++;
    if (s_next_id == BRUCE_SPI_ID_INVALID) s_next_id = 1;
    s_slots[index].id = id;
    s_slots[index].owner = owner;
    s_slots[index].resource_id = resource;
    spi__unlock();
    *out_device = id;
    return BRUCE_OK;
}

bruce_result_t
spi__transfer_internal(bruce_spi_id_t device, const void *tx_data, void *rx_data, size_t size) {
    if (size == 0 || size > BRUCE_SPI_MAX_TRANSFER_SIZE || (tx_data == NULL && rx_data == NULL)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    spi__lock();
    int index = spi__find_locked(device);
    if (index < 0) {
        spi__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    spi_transaction_t transaction = {
        .length = size * 8u,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    bruce_result_t result = spi__esp_result(spi_device_transmit(s_slots[index].handle, &transaction));
    spi__unlock();
    return result;
}

bruce_result_t spi__close_internal(bruce_spi_id_t device) {
    bruce_task_id_t owner = task__current_id();
    spi__lock();
    int index = spi__find_locked(device);
    if (index < 0) {
        spi__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    bruce_resource_id_t resource = s_slots[index].resource_id;
    bool task_owned = s_slots[index].task_owned;
    if (task_owned && s_slots[index].owner != owner) {
        spi__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    spi__release_locked(&s_slots[index]);
    spi__unlock();
    return task_owned ? task_registry__resource_release(resource) : BRUCE_OK;
}

bruce_result_t spi__open(const bruce_spi_device_config_t *config, bruce_spi_id_t *out_device) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK ? spi__open_internal(config, true, out_device) : permission;
}

bruce_result_t spi__transfer(bruce_spi_id_t device, const void *tx_data, void *rx_data, size_t size) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    if (permission != BRUCE_OK) return permission;
    bruce_task_id_t owner = task__current_id();
    spi__lock();
    int index = spi__find_locked(device);
    bool owned = index >= 0 && s_slots[index].task_owned && s_slots[index].owner == owner;
    spi__unlock();
    return owned       ? spi__transfer_internal(device, tx_data, rx_data, size)
           : index < 0 ? BRUCE_ERR_NOT_FOUND
                       : BRUCE_ERR_PERMISSION;
}

bruce_result_t spi__close(bruce_spi_id_t device) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK ? spi__close_internal(device) : permission;
}
