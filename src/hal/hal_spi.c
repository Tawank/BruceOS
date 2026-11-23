#include "hal_spi.h"

bruce_result_t hal_spi_open(const hal_device_t *device, const spi_device_interface_config_t *config,
                            spi_device_handle_t *out) {
    const hal_device_t *bus = device != NULL ? device->bus : NULL;
    if (!hal_device_ready(bus) || bus->driver->class_id != HAL_CLASS_SPI) return BRUCE_ERR_NOT_FOUND;
    const hal_spi_api_t *api = bus->driver->api;
    return api->add_device(bus, config, out);
}

static bruce_result_t hal_spi_master__probe(const hal_device_t *device) {
    const hal_spi_master_config_t *config = device->config;
    if (config == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    spi_bus_config_t bus = {
        .sclk_io_num = config->sck,
        .mosi_io_num = config->mosi,
        .miso_io_num = config->miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = config->max_transfer,
    };
    return spi_bus_initialize((spi_host_device_t)config->host, &bus, SPI_DMA_CH_AUTO) == ESP_OK ? BRUCE_OK
                                                                                               : BRUCE_ERR_IO;
}

static bruce_result_t hal_spi_master__add_device(const hal_device_t *bus, const spi_device_interface_config_t *config,
                                                 spi_device_handle_t *out) {
    const hal_spi_master_config_t *bus_config = bus->config;
    esp_err_t error = spi_bus_add_device((spi_host_device_t)bus_config->host, config, out);
    return error == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
}

static const hal_spi_api_t hal_spi_master__api = {.add_device = hal_spi_master__add_device};

const hal_driver_t hal_spi_master_driver = {
    .name = "spi-master",
    .class_id = HAL_CLASS_SPI,
    .probe = hal_spi_master__probe,
    .api = &hal_spi_master__api,
};
