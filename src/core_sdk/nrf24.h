#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_NRF24_CHANNEL_MIN 0u
#define BRUCE_NRF24_CHANNEL_MAX 125u
#define BRUCE_NRF24_DEFAULT_CHANNEL 76u

typedef struct {
    int sck;
    int miso;
    int mosi;
    int cs;
    int ce;
} bruce_nrf24_pins_t;

/* NRF24 operations require the `rf` permission. The radio attaches to the
 * shared external SPI3_HOST bus and does not use display-owned SPI2_HOST. */
bruce_result_t nrf24__probe(bool *out_connected);
bruce_result_t nrf24__set_channel(uint8_t channel);
bruce_result_t nrf24__get_channel(uint8_t *out_channel);

/* Samples the nRF24 RPD threshold detector on consecutive channels. Each
 * output value is the number of positive samples (0..samples). RPD is a
 * threshold activity indication, not calibrated RSSI or packet decoding. */
bruce_result_t
nrf24__scan(uint8_t first_channel, size_t channel_count, uint8_t samples, uint8_t *out_activity);

void nrf24__get_pins(bruce_nrf24_pins_t *out_pins);
