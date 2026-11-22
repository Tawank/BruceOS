#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief IPv4 address text parsing and formatting.
 *
 * Pure data conversion -- no socket, resource, or permission is involved,
 * unlike tcp.h/udp.h/icmp.h. Existing hostname resolution (tcp__connect(),
 * icmp__ping(), ...) already accepts a literal dotted-quad address directly,
 * so this is for callers that need to do their own address math first (e.g.
 * expanding a CIDR range into individual host addresses to probe).
 */

/**
 * @brief Whether the device can reach the internet now, over Wi-Fi or mobile
 * data. Not permission-gated.
 */
bool net__is_online(void);

/**
 * @brief Starts getting online without waiting.
 *
 * Joins a saved Wi-Fi network in range, or waits for mobile data when it is
 * turned on; the app picks no network, so this needs no `wifi` permission.
 * Returns BRUCE_OK straight away when already online. Follow up with
 * net__connect_poll(). Not permission-gated; the calls that then use the
 * network check their own permissions.
 */
bruce_result_t net__connect_start(void);

/**
 * @brief Result of net__connect_start().
 *
 * BRUCE_OK once online, BRUCE_ERR_BUSY while trying, BRUCE_ERR_TIMEOUT when
 * mobile data did not come up within a minute, otherwise why Wi-Fi failed
 * (BRUCE_ERR_NOT_FOUND when no saved network is in range).
 */
bruce_result_t net__connect_poll(void);

/** Large enough for the longest possible dotted-quad plus a NUL: "255.255.255.255". */
#define BRUCE_NET_IPV4_TEXT_MAX 16

/**
 * @brief Parses a dotted-quad IPv4 address ("192.168.1.1").
 *
 * Rejects anything that is not exactly four decimal octets (0-255) joined by
 * '.', including trailing garbage or a hostname -- use tcp__connect()'s own
 * resolution for that.
 *
 * @param text Dotted-quad text to parse.
 * @param out_address Receives the address in host byte order (0xC0A80101 for "192.168.1.1").
 */
bruce_result_t net__parse_ipv4(const char *text, uint32_t *out_address);

/**
 * @brief Formats a host-byte-order IPv4 address as dotted-quad text.
 *
 * @param address Address in host byte order.
 * @param out Buffer to receive the NUL-terminated text (BRUCE_NET_IPV4_TEXT_MAX is always enough).
 * @param capacity Size of out in bytes.
 */
void net__format_ipv4(uint32_t address, char *out, size_t capacity);
