#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/process.h"

#define BRUCE_TCP_HOST_MAX 64

typedef struct {
    char host[BRUCE_TCP_HOST_MAX];
    uint16_t port;
} bruce_tcp_endpoint_t;

/* TCP handles are owned by the calling process, require the `wifi` permission,
 * and are closed automatically when that process exits. A zero timeout polls.
 * TCP EOF is BRUCE_OK with *out_size == 0. */
bruce_result_t tcp__connect(const char *host, uint16_t port, uint32_t timeout_ms, bruce_tcp_id_t *out_socket);
bruce_result_t tcp__listen(uint16_t port, bruce_tcp_id_t *out_listener);
bruce_result_t tcp__accept(
    bruce_tcp_id_t listener, uint32_t timeout_ms, bruce_tcp_id_t *out_socket, bruce_tcp_endpoint_t *out_peer
);
bruce_result_t
tcp__read(bruce_tcp_id_t socket, void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size);
bruce_result_t
tcp__write(bruce_tcp_id_t socket, const void *buffer, size_t size, uint32_t timeout_ms, size_t *out_size);
bruce_result_t tcp__close(bruce_tcp_id_t socket);
