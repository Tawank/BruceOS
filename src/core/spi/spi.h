#pragma once

#include <stdbool.h>

#include "core_sdk/spi.h"

bruce_result_t
spi__open_internal(const bruce_spi_device_config_t *config, bool task_owned, bruce_spi_id_t *out_device);
bruce_result_t spi__transfer_internal(bruce_spi_id_t device, const void *tx_data, void *rx_data, size_t size);
bruce_result_t spi__close_internal(bruce_spi_id_t device);
