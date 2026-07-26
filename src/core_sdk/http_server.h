#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRUCE_HTTP_SERVER_MAX_ROUTES 8

typedef enum {
    BRUCE_HTTP_SERVER_GET = 0,
    BRUCE_HTTP_SERVER_POST,
    BRUCE_HTTP_SERVER_PUT,
    BRUCE_HTTP_SERVER_DELETE,
} bruce_http_server_method_t;

typedef struct {
    bruce_http_server_method_t method;
    const char *uri;
    const char *content_type;
    const void *body;
    size_t body_len;
    int status_code; /* 0 selects 200. */
} bruce_http_server_route_t;

typedef struct {
    uint16_t port; /* 0 selects port 80. */
    const bruce_http_server_route_t *routes;
    size_t route_count;
} bruce_http_server_options_t;

typedef struct {
    bool running;
    uint16_t port;
    size_t route_count;
} bruce_http_server_status_t;

/* Starts the device-wide HTTP server and copies all route metadata and bodies
 * before returning. Routes currently provide fixed responses, making them
 * safe to serve after the calling task exits. Requires the `http` permission. */
bruce_result_t http_server__start(const bruce_http_server_options_t *options);

/* Stops the server. Returns BRUCE_ERR_BUSY during a lifecycle transition.
 * Requires the `http` permission. */
bruce_result_t http_server__stop(void);

/* Returns a point-in-time server status. Requires the `http` permission. */
bruce_result_t http_server__get_status(bruce_http_server_status_t *out_status);

/* Returns false when denied or stopped. */
bool http_server__is_running(void);

#ifdef __cplusplus
}
#endif
