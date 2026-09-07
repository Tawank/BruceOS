#pragma once

#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief ICMP echo (ping).
 */

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
 * @param out_round_trip_ms Receives the round-trip time in milliseconds on success; may be NULL.
 * @permission wifi
 */
bruce_result_t icmp__ping(const char *host, uint32_t timeout_ms, uint32_t *out_round_trip_ms);
