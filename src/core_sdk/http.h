#pragma once

/**
 * @brief HTTP client.
 *
 * A single synchronous request primitive used by the JavaScript
 * `httpFetch()` binding and any ELF app that holds the `http` permission.
 * `http__request()` does NOT imply the `wifi` permission; callers must
 * arrange Wi-Fi connectivity separately (e.g. via `wifi__connect()`).
 *
 * Response memory is allocated with the process-owned allocator and must be
 * released with `http__response_free()`. Binary bodies may contain embedded
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

    /* Background requests only (http__request_start()): read the body in
     * pieces with http__request_read() as it arrives instead of collecting
     * it whole with http__request_poll(). Core then holds only a small
     * window and pauses the transfer until the caller reads, so bodies of
     * any size up to `max_response_bytes` stream in bounded memory. */
    bool stream;
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

    /* Opaque. Non-NULL when `body` was captured straight into its own
     * PSRAM/swap allocation instead of sharing headers' internal-heap block
     * (a buffered response whose Content-Length fit in
     * memory__external_malloc() - see src/core/http/http.c). Either way,
     * never touch this field directly - just pass the whole response to
     * http__response_free(). */
    const void *body_object;
} bruce_http_response_t;

/**
 * @brief Performs an HTTP request and waits for the response.
 *
 * On success, fills `response` and returns BRUCE_OK. The body is
 * NUL-terminated in buffered mode, but body_len is authoritative. Headers
 * are always one process-owned internal-heap allocation; the body shares it
 * unless it was large enough to instead go through
 * memory__external_malloc() (PSRAM, or swap when no PSRAM is fitted) -
 * either way, release both with a single http__response_free() call. On
 * failure, leaves `response` zeroed and returns a negative BRUCE_ERR_*
 * value.
 *
 * @param request Request to perform (url, method, body, headers, ...).
 * @param response Receives the response on success; zeroed on failure.
 * @permission http
 */
bruce_result_t http__request(const bruce_http_request_t *request, bruce_http_response_t *response);

/* ---- Background requests ------------------------------------------------ */

typedef uint32_t bruce_http_job_id_t;
#define BRUCE_HTTP_JOB_ID_INVALID ((bruce_http_job_id_t)0)

/* Unfinished background requests one process may have at a time. */
#define BRUCE_HTTP_ASYNC_MAX_JOBS 8
#define BRUCE_HTTP_ASYNC_MAX_HEADERS 16

/**
 * @brief Starts `request` in the background and returns at once.
 *
 * Same request fields as http__request(), except `on_response_chunk` must be
 * NULL: the body is buffered (up to `max_response_bytes`) and collected with
 * http__request_poll(), or with `stream` set, read piece by piece with
 * http__request_read(). At most BRUCE_HTTP_ASYNC_MAX_HEADERS header pairs.
 *
 * Requests run on a Core worker pool, up to 4 at once on boards with PSRAM
 * and 1 without; the rest wait in order. Each worker keeps its connection
 * open for about 10 s, so further requests to the same scheme/host/port
 * reuse it instead of repeating the TCP/TLS handshake. Unfinished requests
 * are cancelled when the process exits.
 *
 * Returns BRUCE_ERR_RESOURCE_LIMIT while the caller already has
 * BRUCE_HTTP_ASYNC_MAX_JOBS unfinished requests.
 *
 * @param request Request to perform; copied, so it may be freed on return.
 * @param out_job Receives the job id for http__request_poll()/cancel().
 * @permission http
 */
bruce_result_t http__request_start(const bruce_http_request_t *request, bruce_http_job_id_t *out_job);

/**
 * @brief Collects a buffered background request.
 *
 * Returns BRUCE_ERR_BUSY while it is still queued or running. Otherwise
 * returns the request's result and forgets the job: on BRUCE_OK `response`
 * holds the status code and body (no headers) and must be released with
 * http__response_free(). BRUCE_ERR_NOT_FOUND for an unknown, cancelled, or
 * already collected job, or one owned by another process.
 *
 * @param job Job id from http__request_start().
 * @param response Receives the response once the job is done.
 */
bruce_result_t http__request_poll(bruce_http_job_id_t job, bruce_http_response_t *response);

/**
 * @brief Reads the next piece of a streaming background request (`stream` set).
 *
 * Returns BRUCE_OK with `*out_len` > 0 for body bytes, BRUCE_ERR_BUSY while
 * nothing new has arrived yet, and once the body is fully read, the
 * request's result with `*out_len` == 0 - BRUCE_OK for a complete body - after
 * which the job is forgotten. `*out_status_code` is the HTTP status once the
 * response headers are in, else 0. BRUCE_ERR_INVALID_STATE for a buffered
 * job, which http__request_poll() collects instead.
 *
 * @param job Job id from http__request_start().
 * @param buffer Receives up to `capacity` body bytes.
 * @param capacity Size of buffer in bytes.
 * @param out_len Receives how many bytes were written to buffer.
 * @param out_status_code Optional; receives the HTTP status code.
 */
bruce_result_t http__request_read(
    bruce_http_job_id_t job, void *buffer, size_t capacity, size_t *out_len, int *out_status_code
);

/**
 * @brief Drops a background request; no-op for unknown jobs.
 *
 * A transfer already on the wire finishes in the background and is discarded.
 *
 * @param job Job id from http__request_start().
 */
void http__request_cancel(bruce_http_job_id_t job);

/**
 * @brief Releases an HTTP response.
 *
 * NULL or a zero-initialized response is a no-op.
 *
 * @param response Response to release, previously filled by http__request().
 */
void http__response_free(bruce_http_response_t *response);

#ifdef __cplusplus
}
#endif
