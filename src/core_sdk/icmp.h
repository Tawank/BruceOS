#pragma once

#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief ICMP echo (ping).
 */

/** Largest ICMP payload icmp__ping() will send; a data_size above this fails
 * with BRUCE_ERR_INVALID_ARGUMENT rather than attempting a large allocation. */
#define ICMP__MAX_DATA_SIZE 8192u

/**
 * @brief Sends a single ICMP echo request and waits for a reply.
 *
 * Returns BRUCE_OK if a reply arrived within timeout_ms, BRUCE_ERR_TIMEOUT
 * if none did within that time (the host may be down, unreachable, or
 * simply configured to ignore ICMP -- this cannot tell those apart, the
 * same limitation every ping tool has), BRUCE_ERR_NOT_FOUND if host doesn't
 * resolve, or another BRUCE_ERR_* on failure.
 *
 * @param host Hostname or IPv4 address to ping.
 * @param timeout_ms Time to wait for a reply, in milliseconds (0 uses a 1000ms default).
 * @param data_size Size of the ICMP payload to send, in bytes (0 uses a 64-byte default). Capped at
 *                  ICMP__MAX_DATA_SIZE; larger values fail with BRUCE_ERR_INVALID_ARGUMENT.
 * @param out_round_trip_ms Receives the round-trip time in milliseconds on success; may be NULL.
 * @param out_reply_size Receives the size of the ICMP data in the reply, in bytes, on success; may be NULL.
 * @param out_ttl Receives the reply packet's IP TTL on success; may be NULL.
 * @permission wifi
 */
bruce_result_t icmp__ping(
    const char *host, uint32_t timeout_ms, uint32_t data_size, uint32_t *out_round_trip_ms, uint32_t *out_reply_size,
    uint8_t *out_ttl
);
