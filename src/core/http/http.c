#include "core_sdk/http.h"

#include "core/network/network.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define HTTP__DEFAULT_TIMEOUT_MS 30000
#define HTTP__BUFFER_CHUNK 512
#define HTTP__MAX_HEADER_COUNT 32
#define HTTP__MAX_HEADER_BYTES 4096

typedef struct {
    size_t name_offset;
    size_t value_offset;
} http__header_t;

typedef struct {
    const bruce_http_request_t *request;
    char *arena;
    size_t arena_used;
    size_t arena_capacity;
    size_t body_offset;
    size_t body_len;
    size_t max_response_bytes;
    http__header_t headers[HTTP__MAX_HEADER_COUNT];
    size_t header_count;
    size_t header_bytes;
    bruce_result_t result;
    bool body_started;

    /* Set once a buffered (non-streaming) response's body has moved out of
     * `arena` into its own memory__external_alloc() object (PSRAM, or swap
     * when no PSRAM is fitted) - either up front from a known Content-Length
     * (http__try_start_external_body()) or partway through because `arena`
     * itself ran out of room to grow (http__migrate_body_to_external()),
     * which also covers chunked responses that never had a Content-Length to
     * begin with. While this is non-INVALID, body bytes go straight to it
     * via memory__external_write() instead of `arena`, so a response's body
     * never needs one contiguous block of scarce internal RAM. Its current
     * capacity is body_object.size - 1 (the object always has room for one
     * extra byte: the NUL terminator written in http__build_response()). */
    bruce_memory_object_t body_object;
} http__request_state_t;

static const char *const TAG = "bruce_http";

static esp_http_client_method_t http__method_from_string(const char *method) {
    if (method == NULL || method[0] == '\0') { return HTTP_METHOD_GET; }
    if (strcasecmp(method, "GET") == 0) return HTTP_METHOD_GET;
    if (strcasecmp(method, "POST") == 0) return HTTP_METHOD_POST;
    if (strcasecmp(method, "PUT") == 0) return HTTP_METHOD_PUT;
    if (strcasecmp(method, "DELETE") == 0) return HTTP_METHOD_DELETE;
    if (strcasecmp(method, "HEAD") == 0) return HTTP_METHOD_HEAD;
    if (strcasecmp(method, "PATCH") == 0) return HTTP_METHOD_PATCH;
    if (strcasecmp(method, "OPTIONS") == 0) return HTTP_METHOD_OPTIONS;
    return HTTP_METHOD_GET;
}

static bool http__arena_reserve(http__request_state_t *state, size_t extra) {
    if (extra > SIZE_MAX - state->arena_used) return false;
    size_t required = state->arena_used + extra;
    if (required <= state->arena_capacity) return true;

    size_t arena_limit = HTTP__MAX_HEADER_BYTES;
    if (state->request->on_response_chunk == NULL) {
        arena_limit = state->max_response_bytes > SIZE_MAX - arena_limit
                          ? SIZE_MAX
                          : arena_limit + state->max_response_bytes;
    }
    if (required > arena_limit) return false;

    size_t capacity = state->arena_capacity == 0 ? HTTP__BUFFER_CHUNK : state->arena_capacity;
    while (capacity < required) { capacity = capacity > arena_limit / 2 ? arena_limit : capacity * 2; }
    char *grown = memory__realloc(state->arena, capacity);
    if (grown == NULL) return false;
    state->arena = grown;
    state->arena_capacity = capacity;
    return true;
}

static void http__capture_header(http__request_state_t *state, const char *name, const char *value) {
    if (state->result != BRUCE_OK || state->body_started || state->header_count >= HTTP__MAX_HEADER_COUNT) {
        return;
    }

    size_t name_size = strlen(name) + 1;
    size_t value_size = strlen(value) + 1;
    if (name_size > HTTP__MAX_HEADER_BYTES || value_size > HTTP__MAX_HEADER_BYTES - name_size ||
        state->header_bytes > HTTP__MAX_HEADER_BYTES - name_size - value_size) {
        return;
    }
    if (!http__arena_reserve(state, name_size + value_size)) {
        state->result = BRUCE_ERR_NO_MEMORY;
        return;
    }

    http__header_t *header = &state->headers[state->header_count++];
    header->name_offset = state->arena_used;
    memcpy(state->arena + state->arena_used, name, name_size);
    state->arena_used += name_size;
    header->value_offset = state->arena_used;
    memcpy(state->arena + state->arena_used, value, value_size);
    state->arena_used += value_size;
    state->header_bytes += name_size + value_size;
}

/* Case-insensitive lookup into the headers captured so far. Only valid while
 * state->arena still holds this attempt's data, i.e. before the next
 * http__perform_once() reuses it. */
static const char *http__find_header(const http__request_state_t *state, const char *name) {
    for (size_t i = 0; i < state->header_count; ++i) {
        const char *header_name = state->arena + state->headers[i].name_offset;
        if (strcasecmp(header_name, name) == 0) return state->arena + state->headers[i].value_offset;
    }
    return NULL;
}

static bool http__is_redirect_status(int status_code) {
    return status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 ||
           status_code == 308;
}

/* Parses a decimal Content-Length header value. Rejects anything that isn't
 * purely digits (including empty strings, negative signs, and trailing
 * garbage) so a malformed header falls back to the internal arena instead of
 * being misread. */
static bool http__parse_content_length(const char *value, size_t *out_length) {
    if (value == NULL || value[0] == '\0') return false;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0') return false;
    if (parsed >= SIZE_MAX) return false;
    *out_length = (size_t)parsed;
    return true;
}

/* Called once, at the first body byte of a buffered (non-streaming) response
 * that isn't an intermediate redirect page. When Content-Length is present
 * and fits the caller's max_response_bytes, this gives the body a head
 * start in one memory__external_alloc() object (PSRAM if fitted, otherwise
 * 64 KiB swap pages carved out of flash, falling back to plain internal RAM
 * only if neither is available) sized to it, so a response the internal
 * heap couldn't hold contiguously - e.g. a page bigger than the largest free
 * internal block - doesn't have to grow into PSRAM/swap one doubling step
 * at a time. It's just a head start, not a hard cap: a response without a
 * Content-Length (chunked transfer-encoding, decoded transparently below us
 * by esp_http_client before we ever see it - there's no raw chunk framing to
 * parse here) or one that undercounted it still grows correctly via
 * http__external_body_reserve()/http__migrate_body_to_external() below. On
 * allocation failure this just leaves state->body_object at its zeroed/
 * BRUCE_MEMORY_BACKEND_INVALID default, and the body starts in the internal
 * arena as before. */
static void http__try_start_external_body(http__request_state_t *state) {
    size_t content_length = 0;
    if (!http__parse_content_length(http__find_header(state, "Content-Length"), &content_length)) return;
    if (content_length == 0 || content_length > state->max_response_bytes) return;
    (void)memory__external_alloc(content_length + 1, &state->body_object);
}

/* Grows state->body_object (already established) to fit `state->body_len +
 * extra`, doubling like http__arena_reserve() does for internal memory, but
 * since external objects can't be resized in place, each growth step
 * allocates a new, bigger object and copies the old one's bytes across via a
 * read-only map + a write. `extra` is guaranteed by http__capture_body()'s
 * own max_response_bytes check to keep the required size within it. */
static bool http__external_body_reserve(http__request_state_t *state, size_t extra) {
    size_t capacity = state->body_object.size - 1;
    size_t required = state->body_len + extra;
    if (required <= capacity) return true;

    size_t new_capacity = capacity == 0 ? HTTP__BUFFER_CHUNK : capacity;
    while (new_capacity < required) {
        new_capacity = new_capacity > state->max_response_bytes / 2 ? state->max_response_bytes : new_capacity * 2;
    }
    bruce_memory_object_t grown;
    if (memory__external_alloc(new_capacity + 1, &grown) != BRUCE_OK) return false;
    if (state->body_len > 0) {
        const void *old_data = NULL;
        bruce_result_t result = memory__external_map(&state->body_object, &old_data);
        if (result == BRUCE_OK) result = memory__external_write(&grown, 0, old_data, state->body_len);
        if (result != BRUCE_OK) {
            (void)memory__external_free(&grown);
            return false;
        }
    }
    (void)memory__external_free(&state->body_object);
    state->body_object = grown;
    return true;
}

/* Moves a body so far captured in `arena` (from state->body_offset, for
 * state->body_len bytes) into a fresh external object sized for it plus
 * `extra` more incoming bytes, for when `arena` itself can't grow any
 * further - typically internal RAM fragmentation rather than truly being
 * out of memory, which is exactly the situation PSRAM/swap can route around.
 * This is what lets a chunked response (no Content-Length up front to give
 * http__try_start_external_body() a head start) still end up external once
 * it grows large enough to need it. Leaves `arena` itself untouched either
 * way - it may still hold headers. */
static bool http__migrate_body_to_external(http__request_state_t *state, size_t extra) {
    size_t required = state->body_len + extra;
    size_t new_capacity = required < HTTP__BUFFER_CHUNK ? HTTP__BUFFER_CHUNK : required;
    if (new_capacity > state->max_response_bytes) new_capacity = state->max_response_bytes;
    if (new_capacity < required) return false;

    bruce_memory_object_t object;
    if (memory__external_alloc(new_capacity + 1, &object) != BRUCE_OK) return false;
    if (state->body_len > 0 &&
        memory__external_write(&object, 0, state->arena + state->body_offset, state->body_len) != BRUCE_OK) {
        (void)memory__external_free(&object);
        return false;
    }
    state->body_object = object;
    return true;
}

/* Appends `data_len` buffered-mode body bytes at state->body_len, choosing
 * (and growing, and migrating into) whichever of `arena`/body_object is
 * currently backing the body - see the three helpers above. Does not touch
 * state->body_len itself; the caller does that once for all body-storage
 * modes, streaming included. */
static esp_err_t http__append_body(http__request_state_t *state, const void *data, size_t data_len) {
    if (data_len == 0) return ESP_OK;

    if (state->body_object.backend == BRUCE_MEMORY_BACKEND_INVALID) {
        if (state->body_len == 0) state->body_offset = state->arena_used;
        if (http__arena_reserve(state, data_len)) {
            memcpy(state->arena + state->arena_used, data, data_len);
            state->arena_used += data_len;
            return ESP_OK;
        }
        if (!http__migrate_body_to_external(state, data_len)) {
            state->result = BRUCE_ERR_NO_MEMORY;
            return ESP_FAIL;
        }
    } else if (!http__external_body_reserve(state, data_len)) {
        state->result = BRUCE_ERR_NO_MEMORY;
        return ESP_FAIL;
    }

    bruce_result_t result = memory__external_write(&state->body_object, state->body_len, data, data_len);
    if (result != BRUCE_OK) {
        state->result = result;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t
http__capture_body(http__request_state_t *state, const void *data, size_t data_len, bool is_redirect) {
    if (state->result != BRUCE_OK) return ESP_FAIL;
    if (data_len > state->max_response_bytes - state->body_len) {
        state->result = BRUCE_ERR_RESOURCE_LIMIT;
        return ESP_FAIL;
    }

    if (!state->body_started) {
        state->body_started = true;
        if (!is_redirect && state->request->on_response_chunk == NULL) http__try_start_external_body(state);
    }

    if (state->request->on_response_chunk != NULL) {
        /* An intermediate redirect's body (e.g. a 301 landing page) must
         * never reach the caller's sink - http__request() retries with a
         * fresh state for the next hop, but a streaming sink (wget writing
         * straight to a file) has no way to undo bytes it already saw. */
        if (is_redirect) return ESP_OK;
        bruce_result_t result =
            state->request->on_response_chunk(data, data_len, state->request->response_chunk_context);
        if (result != BRUCE_OK) {
            state->result = result;
            return ESP_FAIL;
        }
    } else {
        esp_err_t err = http__append_body(state, data, data_len);
        if (err != ESP_OK) return err;
    }
    state->body_len += data_len;
    return ESP_OK;
}

static esp_err_t http__event_handler(esp_http_client_event_t *evt) {
    if (evt == NULL || evt->user_data == NULL) return ESP_OK;
    http__request_state_t *state = (http__request_state_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key != NULL && evt->header_value != NULL) {
                http__capture_header(state, evt->header_key, evt->header_value);
            }
            return state->result == BRUCE_OK ? ESP_OK : ESP_FAIL;
        case HTTP_EVENT_ON_DATA: {
            /* The status line and headers are always fully parsed before the
             * first body byte arrives, so both are already available here. */
            bool is_redirect = http__is_redirect_status(esp_http_client_get_status_code(evt->client)) &&
                               http__find_header(state, "Location") != NULL;
            return http__capture_body(state, evt->data, (size_t)evt->data_len, is_redirect);
        }
        default: break;
    }
    return ESP_OK;
}

static bruce_result_t
http__build_response(http__request_state_t *state, int status_code, bruce_http_response_t *response) {
    bool external_body = state->body_object.backend != BRUCE_MEMORY_BACKEND_INVALID;
    bool buffered = state->request->on_response_chunk == NULL;

    size_t pointer_bytes = state->header_count * sizeof(char *) * 2;
    size_t body_bytes = 0;
    if (buffered && !external_body) {
        if (state->body_len == SIZE_MAX) return BRUCE_ERR_NO_MEMORY;
        body_bytes = state->body_len + 1;
    }
    if (state->header_bytes > SIZE_MAX - pointer_bytes ||
        body_bytes > SIZE_MAX - pointer_bytes - state->header_bytes) {
        return BRUCE_ERR_NO_MEMORY;
    }
    /* Headers only, when the body lives in its own external object - see
     * http__try_start_external_body(). */
    size_t storage_size = pointer_bytes + state->arena_used + (body_bytes > 0 ? 1 : 0);
    char *storage = storage_size > 0 ? memory__realloc(state->arena, storage_size) : NULL;
    if (storage_size > 0 && storage == NULL) return BRUCE_ERR_NO_MEMORY;
    if (pointer_bytes > 0) { memmove(storage + pointer_bytes, storage, state->arena_used); }
    state->arena = NULL;

    response->status_code = status_code;
    response->body_len = state->body_len;
    response->header_count = state->header_count;
    if (state->header_count > 0) {
        response->header_names = (char **)storage;
        response->header_values = response->header_names + state->header_count;
    }

    for (size_t i = 0; i < state->header_count; ++i) {
        response->header_names[i] = storage + pointer_bytes + state->headers[i].name_offset;
        response->header_values[i] = storage + pointer_bytes + state->headers[i].value_offset;
    }

    if (external_body) {
        static const uint8_t terminator = 0;
        bruce_result_t result = memory__external_write(&state->body_object, state->body_len, &terminator, 1);
        const void *mapped = NULL;
        if (result == BRUCE_OK) result = memory__external_map(&state->body_object, &mapped);
        if (result != BRUCE_OK) {
            memory__free(storage);
            (void)memory__external_free(&state->body_object);
            return result;
        }
        response->body = (char *)mapped;
        response->body_object = state->body_object;
        state->body_object = (bruce_memory_object_t){0}; /* ownership moved to response */
    } else if (body_bytes > 0) {
        size_t body_offset = state->body_len > 0 ? state->body_offset : state->arena_used;
        response->body = storage + pointer_bytes + body_offset;
        response->body[state->body_len] = '\0';
    }
    return BRUCE_OK;
}

/* One HTTP attempt against `url`/`method`/(`send_body` ? request->body : no
 * body), reusing `state`'s arena. Returns the underlying esp_http_client
 * result; the parsed status code (once known) is written to *out_status_code
 * regardless of that result, since a redirect target can be read out of the
 * response for a request esp_http_client itself reports as failed. */
static esp_err_t http__perform_once(
    const bruce_http_request_t *request, const char *url, const char *method, bool send_body,
    http__request_state_t *state, int *out_status_code
) {
    *out_status_code = -1;
    esp_http_client_config_t config = {
        .url = url,
        .method = http__method_from_string(method),
        .timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : HTTP__DEFAULT_TIMEOUT_MS,
        .cert_pem = NULL,
        /* Validate the server cert against IDF's bundled CA store (mbedtls
         * refuses to proceed at all otherwise: no cert_pem/cacert_buf/
         * use_global_ca_store means there is nothing to check skip_cert_
         * common_name_check against, and TLS setup fails outright). */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Redirects are followed manually below instead: esp_http_client's
         * own auto-redirect has a known gap on a scheme change (e.g. an
         * http:// origin redirecting to https://) - it doesn't reopen the
         * transport as TLS, so it ends up sending a plain-text request at
         * the target's HTTPS port. */
        .disable_auto_redirect = true,
        .event_handler = http__event_handler,
        .user_data = state,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "failed to initialize HTTP client for %s", url);
        return ESP_FAIL;
    }

    for (size_t i = 0; i < request->header_count; ++i) {
        const char *key = request->headers[i * 2];
        const char *value = request->headers[i * 2 + 1];
        if (key != NULL && value != NULL) { esp_http_client_set_header(client, key, value); }
    }

    if (send_body && request->body != NULL && request->body_len > 0) {
        esp_http_client_set_post_field(client, request->body, (int)request->body_len);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) *out_status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

/* Bounds the manual redirect chain below, matching common browser/curl
 * practice. */
#define HTTP__MAX_REDIRECTS 5
#define HTTP__MAX_REDIRECT_URL_LEN 512

bruce_result_t http__request(const bruce_http_request_t *request, bruce_http_response_t *response) {
    if (response == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(response, 0, sizeof(*response));
    if (request == NULL || request->url == NULL || (request->header_count > 0 && request->headers == NULL) ||
        request->header_count > SIZE_MAX / 2 || request->body_len > INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_result_t result = permission__check(BRUCE_PERMISSION_HTTP);
    if (result != BRUCE_OK) return result;

    result = network__init();
    if (result != BRUCE_OK) return result;

    http__request_state_t state = {
        .max_response_bytes = request->max_response_bytes > 0 ? request->max_response_bytes
                                                              : BRUCE_HTTP_DEFAULT_MAX_RESPONSE_BYTES,
    };

    const char *url = request->url;
    const char *method = request->method;
    bool send_body = true;
    char redirect_url[HTTP__MAX_REDIRECT_URL_LEN];
    int status = -1;

    for (int redirects = 0;; ++redirects) {
        state.request = request;
        state.result = BRUCE_OK;
        state.body_started = false;
        state.body_len = 0;
        state.header_count = 0;
        state.header_bytes = 0;
        state.arena_used = 0;
        /* An external body object from a prior hop (e.g. a redirect's own
         * landing page) is superseded by this attempt - release it instead
         * of leaking it until the whole request (or process) ends. */
        if (state.body_object.backend != BRUCE_MEMORY_BACKEND_INVALID) {
            (void)memory__external_free(&state.body_object);
        }

        esp_err_t err = http__perform_once(request, url, method, send_body, &state, &status);
        if (err != ESP_OK || state.result != BRUCE_OK) {
            /* state.result is set by our own capture logic (e.g. running into
             * max_response_bytes or an allocation failure) and can be BRUCE_OK
             * while esp_http_client itself reports the transport failure, or
             * vice versa - log whichever one actually failed instead of
             * always reading esp_err_to_name(err), which is a misleading
             * "ESP_OK" when the transport succeeded but our own state didn't. */
            ESP_LOGE(
                TAG,
                "HTTP request failed for %s: %s",
                url,
                state.result != BRUCE_OK ? result__to_string(state.result) : esp_err_to_name(err)
            );
            memory__free(state.arena);
            if (state.body_object.backend != BRUCE_MEMORY_BACKEND_INVALID) {
                (void)memory__external_free(&state.body_object);
            }
            return state.result != BRUCE_OK ? state.result : BRUCE_ERR_IO;
        }

        const char *location =
            http__is_redirect_status(status) ? http__find_header(&state, "Location") : NULL;
        if (location == NULL || redirects >= HTTP__MAX_REDIRECTS || strstr(location, "://") == NULL) break;
        int written = snprintf(redirect_url, sizeof(redirect_url), "%s", location);
        if (written < 0 || (size_t)written >= sizeof(redirect_url)) break;

        /* 301/302/303 conventionally downgrade a non-HEAD method to GET and
         * drop the body on redirect (the same rule curl/browsers apply);
         * 307/308 preserve both. */
        if (status != 307 && status != 308) {
            if (method == NULL || strcasecmp(method, "HEAD") != 0) method = "GET";
            send_body = false;
        }
        url = redirect_url;
    }

    result = http__build_response(&state, status, response);
    memory__free(state.arena);
    return result;
}

void http__response_free(bruce_http_response_t *response) {
    if (response == NULL) return;
    if (response->body_object.backend != BRUCE_MEMORY_BACKEND_INVALID) {
        /* Headers (if any) still live in their own plain internal-heap
         * block; the body was captured straight into a separate PSRAM/swap
         * object (see http__try_start_external_body()) and needs its own
         * release - freeing it as if it were part of that block would be
         * undefined behaviour. */
        memory__free(response->header_names);
        (void)memory__external_free(&response->body_object);
    } else {
        void *storage =
            response->header_names != NULL ? (void *)response->header_names : (void *)response->body;
        memory__free(storage);
    }
    memset(response, 0, sizeof(*response));
}
