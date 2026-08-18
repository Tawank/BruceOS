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
    while (capacity < required) {
        capacity = capacity > arena_limit / 2 ? arena_limit : capacity * 2;
    }
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

static esp_err_t
http__capture_body(http__request_state_t *state, const void *data, size_t data_len, bool is_redirect) {
    if (state->result != BRUCE_OK) return ESP_FAIL;
    if (data_len > state->max_response_bytes - state->body_len) {
        state->result = BRUCE_ERR_RESOURCE_LIMIT;
        return ESP_FAIL;
    }

    state->body_started = true;
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
    } else if (data_len > 0) {
        if (state->body_len == 0) state->body_offset = state->arena_used;
        if (!http__arena_reserve(state, data_len)) {
            state->result = BRUCE_ERR_NO_MEMORY;
            return ESP_FAIL;
        }
        memcpy(state->arena + state->arena_used, data, data_len);
        state->arena_used += data_len;
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

static bruce_result_t http__build_response(
    http__request_state_t *state, int status_code, bruce_http_response_t *response
) {
    size_t pointer_bytes = state->header_count * sizeof(char *) * 2;
    if (state->request->on_response_chunk == NULL && state->body_len == SIZE_MAX) {
        return BRUCE_ERR_NO_MEMORY;
    }
    size_t body_bytes = state->request->on_response_chunk == NULL ? state->body_len + 1 : 0;
    if (state->header_bytes > SIZE_MAX - pointer_bytes ||
        body_bytes > SIZE_MAX - pointer_bytes - state->header_bytes) {
        return BRUCE_ERR_NO_MEMORY;
    }
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
    if (body_bytes > 0) {
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
    if (request == NULL || request->url == NULL ||
        (request->header_count > 0 && request->headers == NULL) || request->header_count > SIZE_MAX / 2 ||
        request->body_len > INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_result_t result = permission__check(BRUCE_PERMISSION_HTTP);
    if (result != BRUCE_OK) return result;

    result = network__init();
    if (result != BRUCE_OK) return result;

    http__request_state_t state = {
        .max_response_bytes = request->max_response_bytes > 0
                                  ? request->max_response_bytes
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

        esp_err_t err = http__perform_once(request, url, method, send_body, &state, &status);
        if (err != ESP_OK || state.result != BRUCE_OK) {
            ESP_LOGE(TAG, "HTTP request failed for %s: %s", url, esp_err_to_name(err));
            memory__free(state.arena);
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
    void *storage = response->header_names != NULL ? (void *)response->header_names : (void *)response->body;
    memory__free(storage);
    memset(response, 0, sizeof(*response));
}
