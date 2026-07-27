#include "core_sdk/http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/permission.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define HTTP_SERVER__DEFAULT_PORT 80u
#define HTTP_SERVER__URI_MAX_LEN 64u
#define HTTP_SERVER__CONTENT_TYPE_MAX_LEN 64u
#define HTTP_SERVER__MAX_BODY_SIZE (32u * 1024u)

typedef struct {
    char uri[HTTP_SERVER__URI_MAX_LEN];
    char content_type[HTTP_SERVER__CONTENT_TYPE_MAX_LEN];
    void *body;
    size_t body_len;
    char status[32];
} http_server__route_t;

static const char *const TAG = "bruce_http_server";
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static httpd_handle_t s_server;
static bool s_transitioning;
static uint16_t s_port;
static http_server__route_t s_routes[BRUCE_HTTP_SERVER_MAX_ROUTES];
static size_t s_route_count;

static void http_server__lock(void) {
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void http_server__unlock(void) { xSemaphoreGive(s_mutex); }

static void http_server__free_routes(void) {
    for (size_t index = 0; index < s_route_count; ++index) {
        free(s_routes[index].body);
        memset(&s_routes[index], 0, sizeof(s_routes[index]));
    }
    s_route_count = 0;
}

static const char *http_server__reason_phrase(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

static httpd_method_t http_server__method(bruce_http_server_method_t method) {
    switch (method) {
        case BRUCE_HTTP_SERVER_POST: return HTTP_POST;
        case BRUCE_HTTP_SERVER_PUT: return HTTP_PUT;
        case BRUCE_HTTP_SERVER_DELETE: return HTTP_DELETE;
        default: return HTTP_GET;
    }
}

static esp_err_t http_server__route_handler(httpd_req_t *request) {
    const http_server__route_t *route = request->user_ctx;
    httpd_resp_set_status(request, route->status);
    httpd_resp_set_type(request, route->content_type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, route->body, (ssize_t)route->body_len);
}

static bruce_result_t http_server__validate(const bruce_http_server_options_t *options) {
    if (options == NULL || options->routes == NULL || options->route_count == 0 ||
        options->route_count > BRUCE_HTTP_SERVER_MAX_ROUTES) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    size_t total_body_size = 0;
    for (size_t index = 0; index < options->route_count; ++index) {
        const bruce_http_server_route_t *route = &options->routes[index];
        if (route->method < BRUCE_HTTP_SERVER_GET || route->method > BRUCE_HTTP_SERVER_DELETE ||
            route->uri == NULL || route->uri[0] != '/' ||
            strnlen(route->uri, HTTP_SERVER__URI_MAX_LEN) >= HTTP_SERVER__URI_MAX_LEN ||
            route->content_type == NULL || route->content_type[0] == '\0' ||
            strnlen(route->content_type, HTTP_SERVER__CONTENT_TYPE_MAX_LEN) >=
                HTTP_SERVER__CONTENT_TYPE_MAX_LEN ||
            (route->status_code != 0 && (route->status_code < 100 || route->status_code > 599)) ||
            (route->body_len > 0 && route->body == NULL) ||
            route->body_len > HTTP_SERVER__MAX_BODY_SIZE - total_body_size) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        total_body_size += route->body_len;
        for (size_t previous = 0; previous < index; ++previous) {
            if (route->method == options->routes[previous].method &&
                strcmp(route->uri, options->routes[previous].uri) == 0) {
                return BRUCE_ERR_ALREADY_EXISTS;
            }
        }
    }
    return BRUCE_OK;
}

bruce_result_t http_server__start(const bruce_http_server_options_t *options) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_HTTP);
    if (result != BRUCE_OK) return result;
    result = http_server__validate(options);
    if (result != BRUCE_OK) return result;

    http_server__lock();
    if (s_server != NULL || s_transitioning) {
        http_server__unlock();
        return BRUCE_ERR_ALREADY_EXISTS;
    }
    s_transitioning = true;

    for (size_t index = 0; index < options->route_count; ++index) {
        const bruce_http_server_route_t *source = &options->routes[index];
        http_server__route_t *target = &s_routes[index];
        snprintf(target->uri, sizeof(target->uri), "%s", source->uri);
        snprintf(target->content_type, sizeof(target->content_type), "%s", source->content_type);
        target->body_len = source->body_len;
        int status_code = source->status_code == 0 ? 200 : source->status_code;
        snprintf(
            target->status,
            sizeof(target->status),
            "%d %s",
            status_code,
            http_server__reason_phrase(status_code)
        );
        if (source->body_len > 0) {
            target->body = malloc(source->body_len);
            if (target->body == NULL) {
                s_route_count = index + 1u;
                http_server__free_routes();
                s_transitioning = false;
                http_server__unlock();
                return BRUCE_ERR_NO_MEMORY;
            }
            memcpy(target->body, source->body, source->body_len);
        }
        s_route_count = index + 1u;
    }

    uint16_t port = options->port == 0 ? HTTP_SERVER__DEFAULT_PORT : options->port;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server start failed: %s", esp_err_to_name(err));
        http_server__free_routes();
        s_transitioning = false;
        http_server__unlock();
        return BRUCE_ERR_IO;
    }

    s_server = server;
    s_port = port;
    http_server__unlock();

    for (size_t index = 0; index < options->route_count; ++index) {
        const httpd_uri_t uri = {
            .uri = s_routes[index].uri,
            .method = http_server__method(options->routes[index].method),
            .handler = http_server__route_handler,
            .user_ctx = &s_routes[index],
        };
        err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "route registration failed: %s", esp_err_to_name(err));
        httpd_stop(server);
        http_server__lock();
        s_server = NULL;
        s_port = 0;
        http_server__free_routes();
        s_transitioning = false;
        http_server__unlock();
        return BRUCE_ERR_IO;
    }

    http_server__lock();
    s_transitioning = false;
    http_server__unlock();
    ESP_LOGI(TAG, "HTTP server listening on port %u with %u routes", port, (unsigned)options->route_count);
    return BRUCE_OK;
}

bruce_result_t http_server__stop(void) {
    bruce_result_t result = permission__check(BRUCE_PERMISSION_HTTP);
    if (result != BRUCE_OK) return result;

    http_server__lock();
    if (s_transitioning) {
        http_server__unlock();
        return BRUCE_ERR_BUSY;
    }
    httpd_handle_t server = s_server;
    if (server != NULL) s_transitioning = true;
    http_server__unlock();
    if (server == NULL) return BRUCE_OK;

    esp_err_t err = httpd_stop(server);
    http_server__lock();
    if (err == ESP_OK && s_server == server) {
        s_server = NULL;
        s_port = 0;
        http_server__free_routes();
    }
    s_transitioning = false;
    http_server__unlock();
    return err == ESP_OK ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t http_server__get_status(bruce_http_server_status_t *out_status) {
    if (out_status == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = permission__check(BRUCE_PERMISSION_HTTP);
    if (result != BRUCE_OK) return result;
    http_server__lock();
    out_status->running = s_server != NULL;
    out_status->port = s_port;
    out_status->route_count = s_route_count;
    http_server__unlock();
    return BRUCE_OK;
}

bool http_server__is_running(void) {
    if (permission__check(BRUCE_PERMISSION_HTTP) != BRUCE_OK) return false;
    http_server__lock();
    bool running = s_server != NULL;
    http_server__unlock();
    return running;
}
