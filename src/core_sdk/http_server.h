#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRUCE_HTTP_SERVER_MAX_ROUTES 24

typedef enum {
    BRUCE_HTTP_SERVER_GET = 0,
    BRUCE_HTTP_SERVER_POST,
    BRUCE_HTTP_SERVER_PUT,
    BRUCE_HTTP_SERVER_DELETE,
} bruce_http_server_method_t;

/* Opaque request handle. It is valid only for the duration of its route
 * callback and must not be retained or used from another task. */
typedef struct bruce_http_server_request bruce_http_server_request_t;

typedef bruce_result_t (*bruce_http_server_route_callback_t)(
    bruce_http_server_request_t *request,
    void *context
);

typedef struct {
    bruce_http_server_method_t method;
    const char *uri;
    const char *content_type;
    const void *body;
    size_t body_len;
    int status_code; /* 0 selects 200. */
    bruce_http_server_route_callback_t callback;
    void *context;
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

/* Starts the device-wide HTTP server. Each route is either fixed (callback is
 * NULL) or dynamic (callback is non-NULL and content_type/body/body_len/
 * status_code are unset). URI, fixed content type, and fixed body data are
 * copied before this function returns. Callback and context pointer values are
 * retained, not the objects they refer to; their targets must remain valid and
 * unchanged until http_server__stop() succeeds.
 *
 * Dynamic callbacks run in a bounded pool of worker tasks and may execute
 * concurrently, including multiple calls using the same context. Callback
 * contexts and any shared state they access must therefore be thread-safe.
 * Callbacks must not block indefinitely, call http_server__start()/stop(),
 * retain the request, or use it from another task. A callback must return
 * BRUCE_OK after sending one full response, or after finalizing a chunked
 * response. Request headers must be read before the first response send/chunk.
 * Fixed routes remain synchronous. Requires the `http` permission. */
bruce_result_t http_server__start(const bruce_http_server_options_t *options);

/* Request URI includes the path and may include its query suffix. The returned
 * pointer follows the request lifetime described above. */
const char *http_server_request__uri(const bruce_http_server_request_t *request);

/* Returns the raw, undecoded query length, excluding its NUL terminator. */
size_t http_server_request__query_length(const bruce_http_server_request_t *request);

/* Copies the raw, undecoded query and NUL-terminates it. */
bruce_result_t http_server_request__get_query(
    bruce_http_server_request_t *request,
    char *buffer,
    size_t buffer_size
);

/* Returns a header value length, excluding its NUL terminator. Zero also means
 * that the header is absent or empty; get_header distinguishes absence. */
size_t http_server_request__header_length(
    bruce_http_server_request_t *request,
    const char *name
);

/* Copies a request header value and NUL-terminates it. */
bruce_result_t http_server_request__get_header(
    bruce_http_server_request_t *request,
    const char *name,
    char *buffer,
    size_t buffer_size
);

size_t http_server_request__content_length(const bruce_http_server_request_t *request);

/* Reads at most buffer_size bytes and never past content_length. Returns a
 * byte count (zero when exhausted) or a negative BRUCE_ERR_* value. */
int http_server_request__recv(
    bruce_http_server_request_t *request,
    void *buffer,
    size_t buffer_size
);

/* Response metadata must be set before the first send/chunk. String arguments
 * to set_type/set_header must remain valid until that first send/chunk call. */
bruce_result_t http_server_request__set_status(
    bruce_http_server_request_t *request,
    int status_code
);
bruce_result_t http_server_request__set_type(
    bruce_http_server_request_t *request,
    const char *content_type
);
bruce_result_t http_server_request__set_header(
    bruce_http_server_request_t *request,
    const char *name,
    const char *value
);

/* Sends a complete response. data may be NULL only when data_len is zero. */
bruce_result_t http_server_request__send(
    bruce_http_server_request_t *request,
    const void *data,
    size_t data_len
);

/* Sends a non-empty response chunk. Call finalize exactly once afterward. */
bruce_result_t http_server_request__send_chunk(
    bruce_http_server_request_t *request,
    const void *data,
    size_t data_len
);
bruce_result_t http_server_request__finalize(bruce_http_server_request_t *request);

/* Stops the server. Returns BRUCE_ERR_BUSY during a lifecycle transition. If a
 * callback does not finish within the bounded shutdown wait, returns
 * BRUCE_ERR_TIMEOUT with the server and callback contexts still active; keep
 * them valid and call stop again. Requires the `http` permission. */
bruce_result_t http_server__stop(void);

/* Returns a point-in-time server status. Requires the `http` permission. */
bruce_result_t http_server__get_status(bruce_http_server_status_t *out_status);

/* Returns false when denied or stopped. */
bool http_server__is_running(void);

#ifdef __cplusplus
}
#endif
