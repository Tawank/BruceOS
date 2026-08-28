#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

/**
 * @brief SPI device access.
 */

#define BRUCE_SPI_MAX_TRANSFER_SIZE 64u

typedef struct {
    int sck;
    int miso;
    int mosi;
    int cs;
    uint32_t clock_hz;
    uint8_t mode;
} bruce_spi_device_config_t;

/**
 * @brief Opens an SPI device on the board's external hardware SPI bus.
 *
 * Devices with the same SCK/MISO/MOSI tuple share that bus. Device handles
 * belong to the calling process and close automatically on process exit.
 *
 * @param config Device configuration (pins, clock, mode).
 * @param out_device Receives the new device handle.
 * @permission gpio
 */
bruce_result_t spi__open(const bruce_spi_device_config_t *config, bruce_spi_id_t *out_device);

/**
 * @brief Performs a full-duplex SPI transfer.
 *
 * `tx_data` may be NULL for receive-only transfers and `rx_data` may be
 * NULL to discard input.
 *
 * @param device Device handle from spi__open().
 * @param tx_data Bytes to transmit, or NULL for receive-only.
 * @param rx_data Buffer to receive bytes, or NULL to discard input.
 * @param size Number of bytes to transfer.
 * @permission gpio
 */
bruce_result_t spi__transfer(bruce_spi_id_t device, const void *tx_data, void *rx_data, size_t size);

/**
 * @brief Closes an SPI device opened by spi__open().
 *
 * @param device Device handle to close.
 * @permission gpio
 */
bruce_result_t spi__close(bruce_spi_id_t device);
