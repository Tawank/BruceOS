#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/task.h"

#define BRUCE_SPI_MAX_TRANSFER_SIZE 64u

typedef struct {
    int sck;
    int miso;
    int mosi;
    int cs;
    uint32_t clock_hz;
    uint8_t mode;
} bruce_spi_device_config_t;

/* SPI uses the `gpio` permission and the board's external hardware SPI bus.
 * Devices with the same SCK/MISO/MOSI tuple share that bus. Device handles
 * belong to the calling task and close automatically on task exit. `tx_data`
 * may be NULL for receive-only transfers and `rx_data` may be NULL to discard input. */
bruce_result_t spi__open(const bruce_spi_device_config_t *config, bruce_spi_id_t *out_device);
bruce_result_t spi__transfer(bruce_spi_id_t device, const void *tx_data, void *rx_data, size_t size);
bruce_result_t spi__close(bruce_spi_id_t device);
