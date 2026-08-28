#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief nRF24L01(+) radio: channel control and RPD activity scanning.
 */

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

/**
 * @brief Probes for a connected nRF24 radio.
 *
 * The radio attaches to the shared external SPI3_HOST bus and does not use
 * display-owned SPI2_HOST.
 *
 * @param out_connected Receives whether a radio was detected.
 * @permission rf
 */
bruce_result_t nrf24__probe(bool *out_connected);

/**
 * @brief Sets the radio's channel.
 *
 * @param channel New channel (BRUCE_NRF24_CHANNEL_MIN..BRUCE_NRF24_CHANNEL_MAX).
 * @permission rf
 */
bruce_result_t nrf24__set_channel(uint8_t channel);

/**
 * @brief Reads the radio's current channel.
 *
 * @param out_channel Receives the current channel.
 * @permission rf
 */
bruce_result_t nrf24__get_channel(uint8_t *out_channel);

/**
 * @brief Samples the nRF24 RPD threshold detector on consecutive channels.
 *
 * Each output value is the number of positive samples (0..samples). RPD is
 * a threshold activity indication, not calibrated RSSI or packet decoding.
 *
 * @param first_channel First channel to sample.
 * @param channel_count Number of consecutive channels to sample.
 * @param samples Number of samples to take per channel.
 * @param out_activity Array to receive one positive-sample count per channel.
 * @permission rf
 */
bruce_result_t
nrf24__scan(uint8_t first_channel, size_t channel_count, uint8_t samples, uint8_t *out_activity);

/**
 * @brief Returns the board's configured nRF24 SPI/CS/CE pins.
 *
 * @param out_pins Receives the pin configuration.
 */
void nrf24__get_pins(bruce_nrf24_pins_t *out_pins);
