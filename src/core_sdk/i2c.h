#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/task.h"

#define BRUCE_I2C_PORT_AUTO (-1)
#define BRUCE_I2C_MAX_TRANSFER_SIZE 4096u

typedef struct {
    int port;
    int sda;
    int scl;
    uint32_t clock_hz;
    bool enable_internal_pullups;
} bruce_i2c_bus_config_t;

/* I2C uses the `gpio` permission. Bus handles belong to the calling task and
 * close automatically on task exit. Addresses are unshifted 7-bit values.
 * A zero timeout performs one immediate, bounded driver attempt. */
bruce_result_t i2c__open(const bruce_i2c_bus_config_t *config, bruce_i2c_id_t *out_bus);
bruce_result_t i2c__probe(bruce_i2c_id_t bus, uint8_t address, uint32_t timeout_ms,
                          bool *out_present);
bruce_result_t i2c__write(bruce_i2c_id_t bus, uint8_t address, const void *data,
                          size_t size, uint32_t timeout_ms);
bruce_result_t i2c__read(bruce_i2c_id_t bus, uint8_t address, void *data,
                         size_t size, uint32_t timeout_ms);
bruce_result_t i2c__write_read(bruce_i2c_id_t bus, uint8_t address,
                               const void *write_data, size_t write_size,
                               void *read_data, size_t read_size,
                               uint32_t timeout_ms);
bruce_result_t i2c__close(bruce_i2c_id_t bus);
