#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

/**
 * @brief I2C bus access.
 */

#define BRUCE_I2C_PORT_AUTO (-1)
#define BRUCE_I2C_MAX_TRANSFER_SIZE 4096u

typedef struct {
    int port;
    int sda;
    int scl;
    uint32_t clock_hz;
    bool enable_internal_pullups;
} bruce_i2c_bus_config_t;

/**
 * @brief Opens an I2C bus.
 *
 * Bus handles belong to the calling process and close automatically on
 * process exit.
 *
 * @param config Bus configuration (port, pins, clock).
 * @param out_bus Receives the new bus handle.
 * @permission gpio
 */
bruce_result_t i2c__open(const bruce_i2c_bus_config_t *config, bruce_i2c_id_t *out_bus);

/**
 * @brief Probes an I2C bus for a device at `address`.
 *
 * A zero timeout performs one immediate, bounded driver attempt.
 *
 * @param bus Bus handle from i2c__open().
 * @param address Unshifted 7-bit device address.
 * @param timeout_ms Probe timeout in milliseconds, or 0 for one immediate attempt.
 * @param out_present Receives whether a device acknowledged the address.
 * @permission gpio
 */
bruce_result_t i2c__probe(bruce_i2c_id_t bus, uint8_t address, uint32_t timeout_ms, bool *out_present);

/**
 * @brief Writes bytes to an I2C device.
 *
 * @param bus Bus handle from i2c__open().
 * @param address Unshifted 7-bit device address.
 * @param data Bytes to write.
 * @param size Number of bytes to write.
 * @param timeout_ms Transfer timeout in milliseconds, or 0 for one immediate attempt.
 * @permission gpio
 */
bruce_result_t
i2c__write(bruce_i2c_id_t bus, uint8_t address, const void *data, size_t size, uint32_t timeout_ms);

/**
 * @brief Reads bytes from an I2C device.
 *
 * @param bus Bus handle from i2c__open().
 * @param address Unshifted 7-bit device address.
 * @param data Buffer to receive the read bytes.
 * @param size Number of bytes to read.
 * @param timeout_ms Transfer timeout in milliseconds, or 0 for one immediate attempt.
 * @permission gpio
 */
bruce_result_t i2c__read(bruce_i2c_id_t bus, uint8_t address, void *data, size_t size, uint32_t timeout_ms);

/**
 * @brief Writes then reads in one combined I2C transaction (repeated start).
 *
 * @param bus Bus handle from i2c__open().
 * @param address Unshifted 7-bit device address.
 * @param write_data Bytes to write first.
 * @param write_size Number of bytes in write_data.
 * @param read_data Buffer to receive the read bytes.
 * @param read_size Number of bytes to read.
 * @param timeout_ms Transfer timeout in milliseconds, or 0 for one immediate attempt.
 * @permission gpio
 */
bruce_result_t i2c__write_read(
    bruce_i2c_id_t bus, uint8_t address, const void *write_data, size_t write_size, void *read_data,
    size_t read_size, uint32_t timeout_ms
);

/**
 * @brief Closes an I2C bus opened by i2c__open().
 *
 * @param bus Bus handle to close.
 * @permission gpio
 */
bruce_result_t i2c__close(bruce_i2c_id_t bus);
