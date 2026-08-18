#pragma once

/*
 * Public HTTP client API (Core SDK surface).
 *
 * A single synchronous request primitive used by the JavaScript `httpFetch()`
 * binding and any ELF app that holds the `http` permission.  `http__request()`
 * does NOT imply the `wifi` permission; callers must arrange Wi-Fi
 * connectivity separately (e.g. via `wifi__connect()`).
 *
 * Response memory is allocated with the process-owned allocator and must be
 * released with `http__response_free()`.  Binary bodies may contain embedded
 * NUL bytes; callers must use `body_len` instead of `strlen(body)`.
 */

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/memory.h"
#include "core_sdk/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRUCE_HTTP_DEFAULT_MAX_RESPONSE_BYTES (64u * 1024u)

/* Called synchronously as response body chunks arrive. Return BRUCE_OK to
 * continue or any negative BRUCE_ERR_* value to abort the request. The data is
 * valid only for the duration of the callback. */
typedef bruce_result_t (*bruce_http_response_chunk_cb_t)(const void *data, size_t data_len, void *context);

typedef struct {
    const char *url;
    const char *method; /* NULL or GET/POST/PUT/DELETE/HEAD/PATCH/OPTIONS */
    const char *body;
    size_t body_len;

    /* Headers are flattened key/value pairs: headers[0]=key, headers[1]=value,
     * headers[2]=key, ...; `header_count` is the number of pairs. */
    const char *const *headers;
    size_t header_count;

    uint32_t timeout_ms; /* 0 means the implementation default */

    /* Maximum total response-body bytes accepted. 0 selects
     * BRUCE_HTTP_DEFAULT_MAX_RESPONSE_BYTES. The limit applies to buffered and
     * callback responses and returns BRUCE_ERR_RESOURCE_LIMIT when exceeded. */
    size_t max_response_bytes;

    /* When non-NULL, body chunks are delivered here instead of being buffered
     * in the response. `response.body` remains NULL and body_len reports the
     * total bytes delivered. */
    bruce_http_response_chunk_cb_t on_response_chunk;
    void *response_chunk_context;
} bruce_http_request_t;

typedef struct {
    int status_code;
    char *body;
    size_t body_len;

    /* Response headers as parallel name/value arrays. Up to 32 headers and
     * 4096 bytes of header text are retained; excess headers are omitted. */
    char **header_names;
    char **header_values;
    size_t header_count;

    /* Opaque. Set when `body` was captured straight into its own PSRAM/swap
     * object instead of sharing headers' internal-heap block (a buffered
     * response whose Content-Length fit in memory__external_alloc() - see
     * src/core/http/http.c). backend == BRUCE_MEMORY_BACKEND_INVALID (the
     * zero value) means it wasn't; either way, never touch this field
     * directly - just pass the whole response to http__response_free(). */
    bruce_memory_object_t body_object;
} bruce_http_response_t;

/* Perform a synchronous HTTP request. Requires the `http` permission.
 * On success, fills `response` and returns BRUCE_OK. The body is NUL-terminated
 * in buffered mode, but body_len is authoritative. Headers are always one
 * process-owned internal-heap allocation; the body shares it unless it was
 * large enough to instead go through memory__external_alloc() (PSRAM, or
 * swap when no PSRAM is fitted) - either way, release both with a single
 * http__response_free() call. On failure, leaves `response` zeroed and
 * returns a negative BRUCE_ERR_* value. */
bruce_result_t http__request(const bruce_http_request_t *request, bruce_http_response_t *response);

/* Release all memory owned by `response`.  NULL or a zero-initialized response
 * is a no-op. */
void http__response_free(bruce_http_response_t *response);

#ifdef __cplusplus
}
#endif
