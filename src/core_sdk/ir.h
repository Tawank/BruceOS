#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_IR_DEFAULT_FREQUENCY_HZ 38000u
#define BRUCE_IR_MAX_RAW_TIMINGS 512u

/* All operations check the caller's `ir` permission before accessing RMT.
 * File transmission additionally uses the public storage API and therefore
 * requires `storage`. `repeats` means additional complete transmissions after
 * the initial transmission, matching the Bruce IR file/menu convention. */
bruce_result_t ir__transmit_raw(const uint32_t *timings_us, size_t timing_count,
                                uint32_t frequency_hz, uint8_t repeats);

/* Transmits hexadecimal scalar data using NEC, NECext, Samsung32, SIRC,
 * SIRC15, or SIRC20. Protocol matching is case-insensitive. */
bruce_result_t ir__transmit(const char *data_hex, const char *protocol, uint8_t bits,
                            uint8_t repeats);

/* Transmits a parsed Bruce/Flipper address and command pair. Byte strings may
 * be contiguous ("04000000") or space-separated ("04 00 00 00"). */
bruce_result_t ir__transmit_parsed(const char *protocol, const char *address_hex,
                                   const char *command_hex, uint8_t repeats);

/* Receives one signal and writes a complete NUL-terminated version-1 IR file
 * record. Decoded mode recognizes NEC; unrecognized signals return
 * BRUCE_ERR_UNSUPPORTED so callers can retry with raw=true. */
bruce_result_t ir__receive(bool raw, uint32_t timeout_ms, char *out, size_t out_size);

/* Replays every valid parsed/raw record in a Bruce/Flipper version-1 .ir file.
 * Supported parsed records are the same protocols accepted by ir__transmit(). */
bruce_result_t ir__transmit_file(const char *path, uint8_t repeats);

/* Replays one or more version-1 records already held in memory. This is useful
 * for testing a learned capture before it is saved and requires only `ir`. */
bruce_result_t ir__transmit_record(const char *contents, uint8_t repeats);

int ir__tx_pin(void);
int ir__rx_pin(void);
