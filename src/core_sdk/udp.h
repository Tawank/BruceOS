#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

/**
 * @brief UDP datagram sockets.
 *
 * Unlike core_sdk/tcp.h, a UDP socket is connectionless: udp__open() just
 * creates and (optionally) binds one, and every datagram carries its own
 * destination (udp__send_to()) or arrives with its sender attached
 * (udp__receive_from()) rather than there being a single fixed peer for the
 * socket's lifetime.
 */

#define BRUCE_UDP_HOST_MAX 64

typedef struct {
    char host[BRUCE_UDP_HOST_MAX];
    uint16_t port;
} bruce_udp_endpoint_t;

/**
 * @brief Opens a UDP socket.
 *
 * UDP handles are owned by the calling process and are closed
 * automatically when that process exits.
 *
 * @param local_port Local port to bind to, or 0 to let the system assign an
 *                    ephemeral port (the usual choice for a socket that only
 *                    sends, or that expects replies on whatever port it
 *                    sends from).
 * @param out_socket Receives the new socket handle.
 * @permission wifi
 */
bruce_result_t udp__open(uint16_t local_port, bruce_udp_id_t *out_socket);

/**
 * @brief Sends one datagram to a destination.
 *
 * A zero timeout polls. UDP delivery is not guaranteed or ordered; a
 * successful send only means the datagram was handed to the network stack,
 * not that it arrived.
 *
 * @param socket Socket handle from udp__open().
 * @param host Destination hostname or IP address.
 * @param port Destination UDP port.
 * @param buffer Bytes to send.
 * @param size Number of bytes in buffer.
 * @param timeout_ms Send timeout in milliseconds (0 polls).
 * @param out_size Receives the number of bytes sent.
 * @permission wifi
 */
bruce_result_t udp__send_to(
    bruce_udp_id_t socket, const char *host, uint16_t port, const void *buffer, size_t size,
    uint32_t timeout_ms, size_t *out_size
);

/**
 * @brief Receives one datagram.
 *
 * A zero timeout polls. Each call returns at most one datagram; a datagram
 * larger than capacity is truncated to it, with the remainder discarded
 * (standard UDP recv semantics).
 *
 * @param socket Socket handle from udp__open().
 * @param buffer Buffer to receive the datagram.
 * @param capacity Size of buffer in bytes.
 * @param timeout_ms Receive timeout in milliseconds (0 polls).
 * @param out_size Receives the number of bytes received.
 * @param out_peer Receives the sender's address/port. May be NULL.
 * @permission wifi
 */
bruce_result_t udp__receive_from(
    bruce_udp_id_t socket, void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size,
    bruce_udp_endpoint_t *out_peer
);

/**
 * @brief Closes a UDP socket.
 *
 * @param socket Handle to close.
 * @permission wifi
 */
bruce_result_t udp__close(bruce_udp_id_t socket);
