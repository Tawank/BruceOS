#pragma once

#include "driver/spi_master.h" // IWYU pragma: keep
#include "hal.h"

/* Operations of a HAL_CLASS_SPI bus driver. */
typedef struct {
    bruce_result_t (*add_device)(const hal_device_t *bus, const spi_device_interface_config_t *config,
                                 spi_device_handle_t *out);
} hal_spi_api_t;

/* A handle for `device` on its parent SPI bus. `config` gives the clock,
 * mode and chip select. */
bruce_result_t hal_spi_open(const hal_device_t *device, const spi_device_interface_config_t *config,
                            spi_device_handle_t *out);

/* One of the ESP32's SPI hosts, with DMA. Drivers outside the HAL that
 * share the host (SD card, display) find it already set up and only add
 * their chip to it. */
typedef struct {
    uint8_t host; /* SPI2_HOST, SPI3_HOST... */
    int8_t sck;
    int8_t mosi;
    int8_t miso; /* -1: none. */
    uint16_t max_transfer; /* Bytes in one transaction. */
} hal_spi_master_config_t;

extern const hal_driver_t hal_spi_master_driver;

/* The settings of `device`'s parent SPI bus (host and pins), for code that
 * still sets up its own chip on it; NULL when the parent isn't one. */
const hal_spi_master_config_t *hal_spi_bus_config(const hal_device_t *device);
