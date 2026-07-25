#pragma once

/*
 * Public HTTP client API (Core SDK surface).
 *
 * A single synchronous request primitive used by the JavaScript `httpFetch()`
 * binding and any ELF app that holds the `http` permission.  `http__request()`
 * does NOT imply the `wifi` permission; callers must arrange Wi-Fi
 * connectivity separately (e.g. via `wifi__connect()`).
 *
 * Response memory is allocated with the task-owned allocator and must be
 * released with `http__response_free()`.  Binary bodies may contain embedded
 * NUL bytes; callers must use `body_len` instead of `strlen(body)`.
 */

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *url;
    const char *method; /* NULL or "GET"/"POST"/"PUT"/"DELETE"/"HEAD"/"PATCH" */
    const char *body;
    size_t body_len;

    /* Headers are flattened key/value pairs: headers[0]=key, headers[1]=value,
     * headers[2]=key, ...; `header_count` is the number of pairs. */
    const char *const *headers;
    size_t header_count;

    uint32_t timeout_ms; /* 0 means the implementation default */
} bruce_http_request_t;

typedef struct {
    int status_code;
    char *body;
    size_t body_len;

    /* Response headers as flattened key/value pairs, same layout as request. */
    char **header_names;
    char **header_values;
    size_t header_count;
} bruce_http_response_t;

/* Perform a synchronous HTTP request.  Requires the `http` permission.
 * On success, fills `response` and returns BRUCE_OK.  On failure, leaves
 * `response` zeroed and returns a negative BRUCE_ERR_* value. */
bruce_result_t http__request(const bruce_http_request_t *request, bruce_http_response_t *response);

/* Release all memory owned by `response`.  NULL or a zero-initialized response
 * is a no-op. */
void http__response_free(bruce_http_response_t *response);

#ifdef __cplusplus
}
#endif
