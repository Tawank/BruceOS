#include "i2c.h"

#include <string.h>

#include "core/process/process.h"
#include "core_sdk/i2c.h"
#include "core_sdk/permission.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"

#define I2C__MAX_HANDLES 8

typedef struct {
    bool initialized;
    int sda;
    int scl;
    bool pullups;
    size_t references;
    i2c_master_bus_handle_t handle;
} i2c__physical_bus_t;

typedef struct {
    bool in_use;
    bruce_i2c_id_t id;
    bruce_process_id_t owner;
    bruce_resource_id_t resource_id;
    int port;
    uint32_t clock_hz;
} i2c__slot_t;

static i2c__physical_bus_t s_buses[SOC_I2C_NUM];
static i2c__slot_t s_slots[I2C__MAX_HANDLES];
static bruce_i2c_id_t s_next_id = 1;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void i2c__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void i2c__unlock(void) { xSemaphoreGive(s_mutex); }

static bruce_result_t i2c__esp_result(esp_err_t error) {
    if (error == ESP_OK) return BRUCE_OK;
    if (error == ESP_ERR_INVALID_ARG) return BRUCE_ERR_INVALID_ARGUMENT;
    if (error == ESP_ERR_NO_MEM) return BRUCE_ERR_NO_MEMORY;
    if (error == ESP_ERR_TIMEOUT) return BRUCE_ERR_TIMEOUT;
    if (error == ESP_ERR_NOT_FOUND || error == ESP_ERR_INVALID_RESPONSE) return BRUCE_ERR_NOT_FOUND;
    if (error == ESP_ERR_INVALID_STATE) return BRUCE_ERR_BUSY;
    if (error == ESP_ERR_NOT_SUPPORTED) return BRUCE_ERR_UNSUPPORTED;
    return BRUCE_ERR_IO;
}

static int i2c__find_locked(bruce_i2c_id_t id) {
    if (id == BRUCE_I2C_ID_INVALID) return -1;
    for (int i = 0; i < I2C__MAX_HANDLES; ++i) {
        if (s_slots[i].in_use && s_slots[i].id == id) return i;
    }
    return -1;
}

static void i2c__release_locked(i2c__slot_t *slot) {
    i2c__physical_bus_t *bus = &s_buses[slot->port];
    memset(slot, 0, sizeof(*slot));
    if (--bus->references == 0) {
        (void)i2c_del_master_bus(bus->handle);
        memset(bus, 0, sizeof(*bus));
    }
}

static void i2c__cleanup(void *context) {
    i2c__lock();
    i2c__slot_t *slot = context;
    if (slot->in_use) i2c__release_locked(slot);
    i2c__unlock();
}

bruce_result_t i2c__open(const bruce_i2c_bus_config_t *config, bruce_i2c_id_t *out_bus) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    if (permission != BRUCE_OK) return permission;
    bruce_process_id_t owner = process__current_id();
    if (out_bus != NULL) *out_bus = BRUCE_I2C_ID_INVALID;
    if (config == NULL || out_bus == NULL ||
        (config->port != BRUCE_I2C_PORT_AUTO && (config->port < 0 || config->port >= SOC_I2C_NUM)) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->sda) || !GPIO_IS_VALID_OUTPUT_GPIO(config->scl) ||
        config->sda == config->scl || config->clock_hz < 10000u || config->clock_hz > 1000000u) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    i2c__lock();
    int slot_index = -1;
    for (int i = 0; i < I2C__MAX_HANDLES; ++i) {
        if (!s_slots[i].in_use) {
            slot_index = i;
            break;
        }
    }
    if (slot_index < 0) {
        i2c__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    int port = config->port;
    if (port == BRUCE_I2C_PORT_AUTO) {
        port = -1;
        for (int i = 0; i < SOC_I2C_NUM; ++i) {
            if (s_buses[i].initialized && s_buses[i].sda == config->sda && s_buses[i].scl == config->scl &&
                s_buses[i].pullups == config->enable_internal_pullups) {
                port = i;
                break;
            }
        }
        if (port < 0) {
            for (int i = 0; i < SOC_I2C_NUM; ++i) {
                if (!s_buses[i].initialized) {
                    port = i;
                    break;
                }
            }
        }
        if (port < 0) {
            i2c__unlock();
            return BRUCE_ERR_BUSY;
        }
    }

    i2c__physical_bus_t *bus = &s_buses[port];
    if (bus->initialized && (bus->sda != config->sda || bus->scl != config->scl ||
                             bus->pullups != config->enable_internal_pullups)) {
        i2c__unlock();
        return BRUCE_ERR_BUSY;
    }
    if (!bus->initialized) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = port,
            .sda_io_num = config->sda,
            .scl_io_num = config->scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = config->enable_internal_pullups,
        };
        esp_err_t error = i2c_new_master_bus(&bus_config, &bus->handle);
        bruce_result_t result = error == ESP_ERR_NOT_FOUND ? BRUCE_ERR_BUSY : i2c__esp_result(error);
        if (result != BRUCE_OK) {
            i2c__unlock();
            return result;
        }
        bus->initialized = true;
        bus->sda = config->sda;
        bus->scl = config->scl;
        bus->pullups = config->enable_internal_pullups;
    }
    bus->references++;
    s_slots[slot_index].in_use = true;
    s_slots[slot_index].port = port;
    s_slots[slot_index].clock_hz = config->clock_hz;
    i2c__unlock();

    bruce_resource_id_t resource = process_registry__resource_register(i2c__cleanup, &s_slots[slot_index]);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        i2c__cleanup(&s_slots[slot_index]);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    i2c__lock();
    bruce_i2c_id_t id = s_next_id++;
    if (s_next_id == BRUCE_I2C_ID_INVALID) s_next_id = 1;
    s_slots[slot_index].id = id;
    s_slots[slot_index].owner = owner;
    s_slots[slot_index].resource_id = resource;
    i2c__unlock();
    *out_bus = id;
    return BRUCE_OK;
}

static bruce_result_t i2c__device_locked(
    bruce_i2c_id_t id, bruce_process_id_t owner, uint8_t address, i2c_master_dev_handle_t *out_device
) {
    if (address < 0x08 || address > 0x77) return BRUCE_ERR_INVALID_ARGUMENT;
    int index = i2c__find_locked(id);
    if (index < 0) return BRUCE_ERR_NOT_FOUND;
    if (s_slots[index].owner != owner) return BRUCE_ERR_PERMISSION;
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = s_slots[index].clock_hz,
    };
    return i2c__esp_result(i2c_master_bus_add_device(s_buses[s_slots[index].port].handle, &config, out_device)
    );
}

static int i2c__timeout(uint32_t timeout_ms) { return timeout_ms > INT32_MAX ? INT32_MAX : (int)timeout_ms; }

bruce_result_t i2c__probe(bruce_i2c_id_t bus_id, uint8_t address, uint32_t timeout_ms, bool *out_present) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    if (permission != BRUCE_OK) return permission;
    if (out_present == NULL || address < 0x08 || address > 0x77) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_present = false;
    bruce_process_id_t owner = process__current_id();
    i2c__lock();
    int index = i2c__find_locked(bus_id);
    if (index < 0 || s_slots[index].owner != owner) {
        i2c__unlock();
        return index < 0 ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    esp_err_t error =
        i2c_master_probe(s_buses[s_slots[index].port].handle, address, i2c__timeout(timeout_ms));
    i2c__unlock();
    if (error == ESP_OK) {
        *out_present = true;
        return BRUCE_OK;
    }
    if (error == ESP_ERR_NOT_FOUND) return BRUCE_OK;
    return i2c__esp_result(error);
}

static bruce_result_t i2c__transaction(
    bruce_i2c_id_t bus, uint8_t address, const void *write_data, size_t write_size, void *read_data,
    size_t read_size, uint32_t timeout_ms
) {
    if (write_size > BRUCE_I2C_MAX_TRANSFER_SIZE || read_size > BRUCE_I2C_MAX_TRANSFER_SIZE ||
        (write_size > 0 && write_data == NULL) || (read_size > 0 && read_data == NULL) ||
        (write_size == 0 && read_size == 0)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_process_id_t owner = process__current_id();
    i2c__lock();
    i2c_master_dev_handle_t device = NULL;
    bruce_result_t result = i2c__device_locked(bus, owner, address, &device);
    if (result == BRUCE_OK) {
        esp_err_t error;
        if (write_size > 0 && read_size > 0) {
            error = i2c_master_transmit_receive(
                device, write_data, write_size, read_data, read_size, i2c__timeout(timeout_ms)
            );
        } else if (write_size > 0) {
            error = i2c_master_transmit(device, write_data, write_size, i2c__timeout(timeout_ms));
        } else {
            error = i2c_master_receive(device, read_data, read_size, i2c__timeout(timeout_ms));
        }
        result = i2c__esp_result(error);
        if (i2c_master_bus_rm_device(device) != ESP_OK && result == BRUCE_OK) result = BRUCE_ERR_IO;
    }
    i2c__unlock();
    return result;
}

bruce_result_t
i2c__write(bruce_i2c_id_t bus, uint8_t address, const void *data, size_t size, uint32_t timeout_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK ? i2c__transaction(bus, address, data, size, NULL, 0, timeout_ms)
                                  : permission;
}

bruce_result_t i2c__read(bruce_i2c_id_t bus, uint8_t address, void *data, size_t size, uint32_t timeout_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK ? i2c__transaction(bus, address, NULL, 0, data, size, timeout_ms)
                                  : permission;
}

bruce_result_t i2c__write_read(
    bruce_i2c_id_t bus, uint8_t address, const void *write_data, size_t write_size, void *read_data,
    size_t read_size, uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    return permission == BRUCE_OK
               ? i2c__transaction(bus, address, write_data, write_size, read_data, read_size, timeout_ms)
               : permission;
}

bruce_result_t i2c__close(bruce_i2c_id_t bus) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPIO);
    if (permission != BRUCE_OK) return permission;
    bruce_process_id_t owner = process__current_id();
    i2c__lock();
    int index = i2c__find_locked(bus);
    if (index < 0 || s_slots[index].owner != owner) {
        i2c__unlock();
        return index < 0 ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    bruce_resource_id_t resource = s_slots[index].resource_id;
    i2c__release_locked(&s_slots[index]);
    i2c__unlock();
    return process_registry__resource_release(resource);
}
