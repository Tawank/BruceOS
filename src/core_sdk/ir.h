#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Infrared sending and receiving.
 */

#define BRUCE_IR_DEFAULT_FREQUENCY_HZ 38000u
#define BRUCE_IR_MAX_RAW_TIMINGS 512u

/**
 * @brief Transmits raw timing data over RMT.
 *
 * `repeats` means additional complete transmissions after the initial
 * transmission, matching the Bruce IR file/menu convention.
 *
 * @param timings_us Raw mark/space durations in microseconds.
 * @param timing_count Number of entries in timings_us.
 * @param frequency_hz Carrier frequency in Hz, e.g. BRUCE_IR_DEFAULT_FREQUENCY_HZ.
 * @param repeats Additional complete transmissions after the initial one.
 * @permission ir
 */
bruce_result_t
ir__transmit_raw(const uint32_t *timings_us, size_t timing_count, uint32_t frequency_hz, uint8_t repeats);

/**
 * @brief Transmits hexadecimal scalar data using NEC, NECext, Samsung32, SIRC, SIRC15, or SIRC20.
 *
 * Protocol matching is case-insensitive.
 *
 * @param data_hex Scalar data to transmit, as hex text.
 * @param protocol Protocol name, e.g. "NEC" (case-insensitive).
 * @param bits Number of data bits to send.
 * @param repeats Additional complete transmissions after the initial one.
 * @permission ir
 */
bruce_result_t ir__transmit(const char *data_hex, const char *protocol, uint8_t bits, uint8_t repeats);

/**
 * @brief Transmits a parsed Bruce/Flipper address and command pair.
 *
 * Byte strings may be contiguous ("04000000") or space-separated
 * ("04 00 00 00").
 *
 * @param protocol Protocol name.
 * @param address_hex Address bytes, as hex text.
 * @param command_hex Command bytes, as hex text.
 * @param repeats Additional complete transmissions after the initial one.
 * @permission ir
 */
bruce_result_t
ir__transmit_parsed(const char *protocol, const char *address_hex, const char *command_hex, uint8_t repeats);

/**
 * @brief Receives one signal and writes a complete NUL-terminated version-1 IR file record.
 *
 * Decoded mode recognizes NEC; unrecognized signals return
 * BRUCE_ERR_UNSUPPORTED so callers can retry with raw=true.
 *
 * @param raw If true, records raw timings instead of attempting to decode a known protocol.
 * @param timeout_ms Time to wait for a signal, in milliseconds.
 * @param out Buffer to receive the NUL-terminated .ir record.
 * @param out_size Size of out in bytes.
 * @permission ir
 */
bruce_result_t ir__receive(bool raw, uint32_t timeout_ms, char *out, size_t out_size);

/**
 * @brief Replays every valid parsed/raw record in a Bruce/Flipper version-1 .ir file.
 *
 * Supported parsed records are the same protocols accepted by
 * ir__transmit().
 *
 * @param path Path of the .ir file to replay.
 * @param repeats Additional complete transmissions of each record after its initial one.
 * @permission ir, storage
 */
bruce_result_t ir__transmit_file(const char *path, uint8_t repeats);

/**
 * @brief Replays one or more version-1 records already held in memory.
 *
 * This is useful for testing a learned capture before it is saved and
 * requires only `ir`.
 *
 * @param contents Version-1 .ir file records, as text already held in memory.
 * @param repeats Additional complete transmissions of each record after its initial one.
 * @permission ir
 */
bruce_result_t ir__transmit_record(const char *contents, uint8_t repeats);

/** @brief Returns the infrared transmitter pin. */
int ir__tx_pin(void);
/** @brief Returns the infrared receiver pin. */
int ir__rx_pin(void);
