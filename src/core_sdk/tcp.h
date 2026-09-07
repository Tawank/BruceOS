#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

/**
 * @brief TCP network connections.
 */

#define BRUCE_TCP_HOST_MAX 64

typedef struct {
    char host[BRUCE_TCP_HOST_MAX];
    uint16_t port;
} bruce_tcp_endpoint_t;

/**
 * @brief Connects to a TCP endpoint.
 *
 * TCP handles are owned by the calling process and are closed
 * automatically when that process exits.
 *
 * @param host Hostname or IP address to connect to.
 * @param port TCP port to connect to.
 * @param timeout_ms Connection timeout in milliseconds (0 polls).
 * @param out_socket Receives the new socket handle.
 * @permission wifi
 */
bruce_result_t tcp__connect(const char *host, uint16_t port, uint32_t timeout_ms, bruce_tcp_id_t *out_socket);

/**
 * @brief Connects to a TCP endpoint from a specific local port.
 *
 * Same as tcp__connect(), but binds the local end of the connection to
 * `local_port` before connecting (0 behaves exactly like tcp__connect() --
 * an OS-assigned ephemeral port). Useful for reaching a peer whose firewall
 * trusts traffic from a specific source port. Fails with BRUCE_ERR_IO if
 * `local_port` is already bound by something else.
 *
 * @param host Hostname or IP address to connect to.
 * @param port TCP port to connect to.
 * @param local_port Local port to bind before connecting, or 0 for an OS-assigned ephemeral port.
 * @param timeout_ms Connection timeout in milliseconds (0 polls).
 * @param out_socket Receives the new socket handle.
 * @permission wifi
 */
bruce_result_t tcp__connect_from(
    const char *host, uint16_t port, uint16_t local_port, uint32_t timeout_ms, bruce_tcp_id_t *out_socket
);

/**
 * @brief Starts listening for inbound TCP connections on a port.
 *
 * @param port TCP port to listen on.
 * @param out_listener Receives the new listener handle.
 * @permission wifi
 */
bruce_result_t tcp__listen(uint16_t port, bruce_tcp_id_t *out_listener);

/**
 * @brief Accepts one inbound connection on a listener.
 *
 * A zero timeout polls.
 *
 * @param listener Listener handle from tcp__listen().
 * @param timeout_ms Time to wait for a connection, in milliseconds (0 polls).
 * @param out_socket Receives the new connected socket handle.
 * @param out_peer Receives the peer's address/port.
 * @permission wifi
 */
bruce_result_t tcp__accept(
    bruce_tcp_id_t listener, uint32_t timeout_ms, bruce_tcp_id_t *out_socket, bruce_tcp_endpoint_t *out_peer
);

/**
 * @brief Reads from a connected TCP socket.
 *
 * A zero timeout polls. TCP EOF is BRUCE_OK with *out_size == 0.
 *
 * @param socket Connected socket handle.
 * @param buffer Buffer to receive read bytes.
 * @param capacity Size of buffer in bytes.
 * @param timeout_ms Read timeout in milliseconds (0 polls).
 * @param out_size Receives the number of bytes read.
 * @permission wifi
 */
bruce_result_t
tcp__read(bruce_tcp_id_t socket, void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size);

/**
 * @brief Writes to a connected TCP socket.
 *
 * A zero timeout polls.
 *
 * @param socket Connected socket handle.
 * @param buffer Bytes to write.
 * @param size Number of bytes in buffer.
 * @param timeout_ms Write timeout in milliseconds (0 polls).
 * @param out_size Receives the number of bytes written.
 * @permission wifi
 */
bruce_result_t
tcp__write(bruce_tcp_id_t socket, const void *buffer, size_t size, uint32_t timeout_ms, size_t *out_size);

/**
 * @brief Closes a TCP socket or listener.
 *
 * @param socket Handle to close.
 * @permission wifi
 */
bruce_result_t tcp__close(bruce_tcp_id_t socket);
