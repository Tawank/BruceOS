#include "core_sdk/http.h"

#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"

#include "esp_http_client.h"
#include "esp_log.h"

#define HTTP__DEFAULT_TIMEOUT_MS 30000
#define HTTP__BUFFER_CHUNK 512
#define HTTP__MAX_HEADER_COUNT 32
#define HTTP__HEADER_NAME_MAX_LEN 64
#define HTTP__HEADER_VALUE_MAX_LEN 256

typedef struct {
    char names[HTTP__MAX_HEADER_COUNT][HTTP__HEADER_NAME_MAX_LEN];
    char values[HTTP__MAX_HEADER_COUNT][HTTP__HEADER_VALUE_MAX_LEN];
    size_t count;
} http__headers_t;

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

static esp_err_t http__event_handler(esp_http_client_event_t *evt) {
    if (evt == NULL) return ESP_OK;
    http__headers_t *headers = (http__headers_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (headers != NULL && evt->header_key != NULL && evt->header_value != NULL &&
                headers->count < HTTP__MAX_HEADER_COUNT) {
                size_t name_len = strlen(evt->header_key);
                size_t value_len = strlen(evt->header_value);
                if (name_len < HTTP__HEADER_NAME_MAX_LEN && value_len < HTTP__HEADER_VALUE_MAX_LEN) {
                    memcpy(headers->names[headers->count], evt->header_key, name_len + 1);
                    memcpy(headers->values[headers->count], evt->header_value, value_len + 1);
                    headers->count++;
                }
            }
            break;
        default: break;
    }
    return ESP_OK;
}

static bruce_result_t http__collect_headers(const http__headers_t *headers, bruce_http_response_t *response) {
    if (headers->count == 0) return BRUCE_OK;

    response->header_names = memory__malloc(sizeof(char *) * headers->count);
    response->header_values = memory__malloc(sizeof(char *) * headers->count);
    if (response->header_names == NULL || response->header_values == NULL) {
        memory__free(response->header_names);
        memory__free(response->header_values);
        response->header_names = NULL;
        response->header_values = NULL;
        return BRUCE_ERR_NO_MEMORY;
    }

    for (size_t i = 0; i < headers->count; ++i) {
        size_t name_len = strlen(headers->names[i]);
        size_t value_len = strlen(headers->values[i]);
        response->header_names[i] = memory__malloc(name_len + 1);
        response->header_values[i] = memory__malloc(value_len + 1);
        if (response->header_names[i] == NULL || response->header_values[i] == NULL) {
            for (size_t j = 0; j < i; ++j) {
                memory__free(response->header_names[j]);
                memory__free(response->header_values[j]);
            }
            memory__free(response->header_names);
            memory__free(response->header_values);
            response->header_names = NULL;
            response->header_values = NULL;
            return BRUCE_ERR_NO_MEMORY;
        }
        memcpy(response->header_names[i], headers->names[i], name_len + 1);
        memcpy(response->header_values[i], headers->values[i], value_len + 1);
    }
    response->header_count = headers->count;
    return BRUCE_OK;
}

bruce_result_t http__request(const bruce_http_request_t *request, bruce_http_response_t *response) {
    if (request == NULL || response == NULL || request->url == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bruce_result_t result = permission__check(BRUCE_PERMISSION_HTTP);
    if (result != BRUCE_OK) return result;

    memset(response, 0, sizeof(*response));

    http__headers_t headers = {0};

    esp_http_client_config_t config = {
        .url = request->url,
        .method = http__method_from_string(request->method),
        .timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : HTTP__DEFAULT_TIMEOUT_MS,
        .cert_pem = NULL,
        .skip_cert_common_name_check = true,
        .event_handler = http__event_handler,
        .user_data = &headers,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "failed to initialize HTTP client for %s", request->url);
        return BRUCE_ERR_IO;
    }

    for (size_t i = 0; i < request->header_count; ++i) {
        const char *key = request->headers[i * 2];
        const char *value = request->headers[i * 2 + 1];
        if (key != NULL && value != NULL) { esp_http_client_set_header(client, key, value); }
    }

    if (request->body != NULL && request->body_len > 0) {
        esp_http_client_set_post_field(client, request->body, (int)request->body_len);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed for %s: %s", request->url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return BRUCE_ERR_IO;
    }

    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    if (content_length < 0) content_length = 0;

    char *body = NULL;
    size_t body_used = 0;
    size_t body_capacity = 0;

    if (content_length > 0) {
        body_capacity = (size_t)content_length + 1;
        body = memory__malloc(body_capacity);
        if (body == NULL) {
            esp_http_client_cleanup(client);
            return BRUCE_ERR_NO_MEMORY;
        }
    }

    for (;;) {
        if (body_capacity - body_used < HTTP__BUFFER_CHUNK) {
            size_t new_capacity = body_capacity == 0 ? HTTP__BUFFER_CHUNK : body_capacity * 2;
            char *grown = memory__malloc(new_capacity);
            if (grown == NULL) {
                memory__free(body);
                esp_http_client_cleanup(client);
                return BRUCE_ERR_NO_MEMORY;
            }
            if (body != NULL) {
                memcpy(grown, body, body_used);
                memory__free(body);
            }
            body = grown;
            body_capacity = new_capacity;
        }

        int read = esp_http_client_read(client, body + body_used, (int)(body_capacity - body_used - 1));
        if (read < 0) {
            ESP_LOGE(TAG, "HTTP read failed for %s", request->url);
            memory__free(body);
            esp_http_client_cleanup(client);
            return BRUCE_ERR_IO;
        }
        if (read == 0) break;
        body_used += (size_t)read;
    }
    if (body != NULL) { body[body_used] = '\0'; }

    response->status_code = status;
    response->body = body;
    response->body_len = body_used;

    result = http__collect_headers(&headers, response);
    if (result != BRUCE_OK) {
        memory__free(body);
        response->body = NULL;
        response->body_len = 0;
        esp_http_client_cleanup(client);
        return result;
    }

    esp_http_client_cleanup(client);
    return BRUCE_OK;
}

void http__response_free(bruce_http_response_t *response) {
    if (response == NULL) return;
    memory__free(response->body);
    response->body = NULL;
    response->body_len = 0;
    if (response->header_names != NULL) {
        for (size_t i = 0; i < response->header_count; ++i) { memory__free(response->header_names[i]); }
        memory__free(response->header_names);
    }
    if (response->header_values != NULL) {
        for (size_t i = 0; i < response->header_count; ++i) { memory__free(response->header_values[i]); }
        memory__free(response->header_values);
    }
    response->header_names = NULL;
    response->header_values = NULL;
    response->header_count = 0;
}
