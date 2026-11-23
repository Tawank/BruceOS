#include "hal_i2c.h"

bruce_result_t hal_i2c_open(const hal_device_t *device, uint32_t speed_hz, i2c_master_dev_handle_t *out) {
    const hal_device_t *bus = device != NULL ? device->bus : NULL;
    if (!hal_device_ready(bus) || bus->driver->class_id != HAL_CLASS_I2C) return BRUCE_ERR_NOT_FOUND;
    const hal_i2c_api_t *api = bus->driver->api;
    return api->add_device(bus, device->address, speed_hz, out);
}

i2c_master_bus_handle_t hal_i2c_bus(const hal_device_t *bus) {
    if (!hal_device_ready(bus) || bus->driver != &hal_i2c_master_driver) return NULL;
    return bus->state->data;
}

static bruce_result_t hal_i2c_master__probe(const hal_device_t *device) {
    const hal_i2c_master_config_t *config = device->config;
    if (config == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = config->port,
        .sda_io_num = (gpio_num_t)config->sda,
        .scl_io_num = (gpio_num_t)config->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = config->pullup,
    };
    i2c_master_bus_handle_t handle = NULL;
    if (i2c_new_master_bus(&bus_config, &handle) != ESP_OK) return BRUCE_ERR_IO;
    device->state->data = handle;
    return BRUCE_OK;
}

static bruce_result_t hal_i2c_master__add_device(const hal_device_t *bus, uint16_t address, uint32_t speed_hz,
                                                 i2c_master_dev_handle_t *out) {
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = speed_hz,
    };
    return i2c_master_bus_add_device(bus->state->data, &config, out) == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
}

static const hal_i2c_api_t hal_i2c_master__api = {.add_device = hal_i2c_master__add_device};

const hal_driver_t hal_i2c_master_driver = {
    .name = "i2c-master",
    .class_id = HAL_CLASS_I2C,
    .probe = hal_i2c_master__probe,
    .api = &hal_i2c_master__api,
};
