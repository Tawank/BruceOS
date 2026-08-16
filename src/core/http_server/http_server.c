#include "core_sdk/http_server.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/permission.h"
#include "core_sdk/process.h"

#include "core/process/process.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HTTP_SERVER__DEFAULT_PORT 80u
#define HTTP_SERVER__URI_MAX_LEN 64u
#define HTTP_SERVER__CONTENT_TYPE_MAX_LEN 64u
#define HTTP_SERVER__MAX_BODY_SIZE (32u * 1024u)
#define HTTP_SERVER__WORKER_COUNT 2u
#define HTTP_SERVER__WORKER_QUEUE_LENGTH HTTP_SERVER__WORKER_COUNT
/* Above the 4096-byte Core process default: workers are now
 * process_registry-tracked processes (see http_server__start_workers()), so
 * every route callback's frame sits on top of process bookkeeping calls
 * (memory__malloc()/memory__external_alloc() et al.) that didn't used to run
 * on this stack. */
#define HTTP_SERVER__WORKER_STACK_SIZE 6144u
#define HTTP_SERVER__WORKER_PRIORITY 5u
#define HTTP_SERVER__WORKER_POLL_MS 50u
#define HTTP_SERVER__WORKER_STOP_TIMEOUT_MS 5000u

typedef struct {
    char uri[HTTP_SERVER__URI_MAX_LEN];
    char content_type[HTTP_SERVER__CONTENT_TYPE_MAX_LEN];
    void *body;
    size_t body_len;
    char status[32];
    bruce_http_server_method_t method;
    bruce_http_server_route_callback_t callback;
    void *context;
} http_server__route_t;

typedef struct {
    httpd_req_t *request;
    const http_server__route_t *route;
} http_server__job_t;

typedef enum {
    HTTP_SERVER__RESPONSE_NONE = 0,
    HTTP_SERVER__RESPONSE_CHUNKING,
    HTTP_SERVER__RESPONSE_COMPLETE,
} http_server__response_state_t;

struct bruce_http_server_request {
    httpd_req_t *request;
    size_t received;
    http_server__response_state_t response_state;
    char status[32];
};

static const char *const TAG = "bruce_http_server";
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static httpd_handle_t s_server;
static bool s_transitioning;
static uint16_t s_port;
static http_server__route_t *s_routes;
static size_t s_route_count;
static QueueHandle_t s_worker_queue;
static SemaphoreHandle_t s_worker_capacity;
static SemaphoreHandle_t s_worker_exited;
static bruce_process_id_t s_worker_process_ids[HTTP_SERVER__WORKER_COUNT];
static size_t s_workers_remaining;
static bool s_workers_accepting;
static atomic_bool s_workers_stopping;

static void http_server__lock(void) {
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void http_server__unlock(void) { xSemaphoreGive(s_mutex); }

static void http_server__free_routes(void) {
    for (size_t index = 0; index < s_route_count; ++index) {
        free(s_routes[index].body);
    }
    free(s_routes);
    s_routes = NULL;
    s_route_count = 0;
}

static esp_err_t http_server__send_unavailable(httpd_req_t *request) {
    static const char body[] = "HTTP server is busy";
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_send(request, body, sizeof(body) - 1u);
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

static bruce_result_t http_server__esp_result(esp_err_t err) {
    if (err == ESP_OK) return BRUCE_OK;
    if (err == ESP_ERR_INVALID_ARG) return BRUCE_ERR_INVALID_ARGUMENT;
    if (err == ESP_ERR_NOT_FOUND) return BRUCE_ERR_NOT_FOUND;
    if (err == ESP_ERR_NO_MEM) return BRUCE_ERR_NO_MEMORY;
    if (err == ESP_ERR_HTTPD_RESULT_TRUNC) return BRUCE_ERR_RESOURCE_LIMIT;
    return BRUCE_ERR_IO;
}

static bool http_server__can_set_response(const bruce_http_server_request_t *request) {
    return request != NULL && request->request != NULL &&
           request->response_state == HTTP_SERVER__RESPONSE_NONE;
}

const char *http_server_request__uri(const bruce_http_server_request_t *request) {
    return request != NULL && request->request != NULL ? request->request->uri : NULL;
}

size_t http_server_request__query_length(const bruce_http_server_request_t *request) {
    return request != NULL && request->request != NULL
               ? httpd_req_get_url_query_len(request->request)
               : 0;
}

bruce_result_t http_server_request__get_query(
    bruce_http_server_request_t *request,
    char *buffer,
    size_t buffer_size
) {
    if (request == NULL || request->request == NULL || buffer == NULL || buffer_size == 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return http_server__esp_result(httpd_req_get_url_query_str(request->request, buffer, buffer_size));
}

size_t http_server_request__header_length(
    bruce_http_server_request_t *request,
    const char *name
) {
    if (request == NULL || request->request == NULL || name == NULL) return 0;
    return httpd_req_get_hdr_value_len(request->request, name);
}

bruce_result_t http_server_request__get_header(
    bruce_http_server_request_t *request,
    const char *name,
    char *buffer,
    size_t buffer_size
) {
    if (request == NULL || request->request == NULL || name == NULL || name[0] == '\0' ||
        buffer == NULL || buffer_size == 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return http_server__esp_result(
        httpd_req_get_hdr_value_str(request->request, name, buffer, buffer_size)
    );
}

size_t http_server_request__content_length(const bruce_http_server_request_t *request) {
    return request != NULL && request->request != NULL ? request->request->content_len : 0;
}

int http_server_request__recv(
    bruce_http_server_request_t *request,
    void *buffer,
    size_t buffer_size
) {
    if (request == NULL || request->request == NULL || (buffer == NULL && buffer_size > 0) ||
        request->response_state != HTTP_SERVER__RESPONSE_NONE) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    size_t remaining = request->request->content_len - request->received;
    if (buffer_size > remaining) buffer_size = remaining;
    if (buffer_size > INT_MAX) buffer_size = INT_MAX;
    if (buffer_size == 0) return 0;
    int received = httpd_req_recv(request->request, buffer, buffer_size);
    if (received > 0) {
        request->received += (size_t)received;
        return received;
    }
    if (received == HTTPD_SOCK_ERR_TIMEOUT) return BRUCE_ERR_TIMEOUT;
    return BRUCE_ERR_IO;
}

bruce_result_t http_server_request__set_status(
    bruce_http_server_request_t *request,
    int status_code
) {
    if (!http_server__can_set_response(request) || status_code < 100 || status_code > 599) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    snprintf(
        request->status,
        sizeof(request->status),
        "%d %s",
        status_code,
        http_server__reason_phrase(status_code)
    );
    return http_server__esp_result(httpd_resp_set_status(request->request, request->status));
}

bruce_result_t http_server_request__set_type(
    bruce_http_server_request_t *request,
    const char *content_type
) {
    if (!http_server__can_set_response(request) || content_type == NULL || content_type[0] == '\0') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return http_server__esp_result(httpd_resp_set_type(request->request, content_type));
}

bruce_result_t http_server_request__set_header(
    bruce_http_server_request_t *request,
    const char *name,
    const char *value
) {
    if (!http_server__can_set_response(request) || name == NULL || name[0] == '\0' || value == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return http_server__esp_result(httpd_resp_set_hdr(request->request, name, value));
}

bruce_result_t http_server_request__send(
    bruce_http_server_request_t *request,
    const void *data,
    size_t data_len
) {
    if (!http_server__can_set_response(request) || (data == NULL && data_len > 0) ||
        data_len > INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    esp_err_t err = httpd_resp_send(request->request, data, (ssize_t)data_len);
    if (err == ESP_OK) request->response_state = HTTP_SERVER__RESPONSE_COMPLETE;
    return http_server__esp_result(err);
}

bruce_result_t http_server_request__send_chunk(
    bruce_http_server_request_t *request,
    const void *data,
    size_t data_len
) {
    if (request == NULL || request->request == NULL ||
        request->response_state == HTTP_SERVER__RESPONSE_COMPLETE || data == NULL || data_len == 0 ||
        data_len > INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    esp_err_t err = httpd_resp_send_chunk(request->request, data, (ssize_t)data_len);
    if (err == ESP_OK) request->response_state = HTTP_SERVER__RESPONSE_CHUNKING;
    return http_server__esp_result(err);
}

bruce_result_t http_server_request__finalize(bruce_http_server_request_t *request) {
    if (request == NULL || request->request == NULL ||
        request->response_state != HTTP_SERVER__RESPONSE_CHUNKING) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    esp_err_t err = httpd_resp_send_chunk(request->request, NULL, 0);
    if (err == ESP_OK) request->response_state = HTTP_SERVER__RESPONSE_COMPLETE;
    return http_server__esp_result(err);
}

static esp_err_t http_server__invoke_dynamic(
    httpd_req_t *request,
    const http_server__route_t *route
) {
    bruce_http_server_request_t public_request = {
        .request = request,
    };
    bruce_result_t result = route->callback(&public_request, route->context);
    if (result == BRUCE_OK && public_request.response_state == HTTP_SERVER__RESPONSE_COMPLETE) {
        return ESP_OK;
    }
    if (public_request.response_state == HTTP_SERVER__RESPONSE_NONE) {
        static const char error_body[] = "Dynamic route did not complete a response";
        httpd_resp_set_status(request, "500 Internal Server Error");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_send(request, error_body, sizeof(error_body) - 1u);
    }
    return ESP_FAIL;
}

/* Runs as a process_registry-tracked process (see http_server__start_workers())
 * rather than a bare FreeRTOS task, so route callbacks -- which run
 * synchronously on this stack via http_server__invoke_dynamic() below -- get
 * a real current-process context. That's what lets webui's route handlers
 * call memory__malloc()/memory__external_alloc(): both require
 * process__current_id() to resolve, which only happens on a task
 * process_registry__create() itself spawned. Returning (rather than calling
 * vTaskDelete()) lets process__trampoline() tear the process down normally. */
static int http_server__worker(void *argument) {
    size_t index = (size_t)(uintptr_t)argument;
    for (;;) {
        http_server__job_t job;
        if (xQueueReceive(
                s_worker_queue,
                &job,
                pdMS_TO_TICKS(HTTP_SERVER__WORKER_POLL_MS)
            ) == pdTRUE) {
            http_server__invoke_dynamic(job.request, job.route);
            esp_err_t err = httpd_req_async_handler_complete(job.request);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "async request completion failed: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(s_worker_capacity);
            continue;
        }
        if (atomic_load(&s_workers_stopping)) break;
    }
    s_worker_process_ids[index] = BRUCE_PROCESS_ID_INVALID;
    xSemaphoreGive(s_worker_exited);
    return 0;
}

static void http_server__release_workers(void) {
    http_server__job_t job;
    while (s_worker_queue != NULL && xQueueReceive(s_worker_queue, &job, 0) == pdTRUE) {
        http_server__send_unavailable(job.request);
        httpd_req_async_handler_complete(job.request);
    }
    if (s_worker_queue != NULL) vQueueDelete(s_worker_queue);
    if (s_worker_capacity != NULL) vSemaphoreDelete(s_worker_capacity);
    if (s_worker_exited != NULL) vSemaphoreDelete(s_worker_exited);
    s_worker_queue = NULL;
    s_worker_capacity = NULL;
    s_worker_exited = NULL;
    s_workers_remaining = 0;
}

static bruce_result_t http_server__wait_for_workers(TickType_t timeout) {
    atomic_store(&s_workers_stopping, true);
    TickType_t deadline = xTaskGetTickCount() + timeout;
    while (s_workers_remaining > 0) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (int32_t)(deadline - now) > 0 ? deadline - now : 0;
        if (xSemaphoreTake(s_worker_exited, wait) != pdTRUE) return BRUCE_ERR_TIMEOUT;
        --s_workers_remaining;
    }
    http_server__release_workers();
    return BRUCE_OK;
}

static void http_server__abort_idle_workers(void) {
    if (s_worker_queue == NULL) return;
    if (http_server__wait_for_workers(pdMS_TO_TICKS(1000)) == BRUCE_OK) return;
    for (size_t worker = 0; worker < HTTP_SERVER__WORKER_COUNT; ++worker) {
        if (s_worker_process_ids[worker] != BRUCE_PROCESS_ID_INVALID) {
            /* Force-kill rather than vTaskDelete(): the worker is a tracked
             * process now, and process__kill() releases whatever
             * process_registry resources (memory__malloc/memory__external_
             * alloc allocations, mainly) it still owned instead of leaking
             * them. */
            (void)process__kill(s_worker_process_ids[worker]);
            s_worker_process_ids[worker] = BRUCE_PROCESS_ID_INVALID;
        }
    }
    http_server__release_workers();
}

static bruce_result_t http_server__start_workers(void) {
    s_worker_queue = xQueueCreate(HTTP_SERVER__WORKER_QUEUE_LENGTH, sizeof(http_server__job_t));
    s_worker_capacity = xSemaphoreCreateCounting(
        HTTP_SERVER__WORKER_COUNT,
        HTTP_SERVER__WORKER_COUNT
    );
    s_worker_exited = xSemaphoreCreateCounting(HTTP_SERVER__WORKER_COUNT, 0);
    if (s_worker_queue == NULL || s_worker_capacity == NULL || s_worker_exited == NULL) {
        http_server__release_workers();
        return BRUCE_ERR_NO_MEMORY;
    }

    atomic_store(&s_workers_stopping, false);
    s_workers_remaining = 0;
    for (size_t index = 0; index < HTTP_SERVER__WORKER_COUNT; ++index) {
        char name[BRUCE_PROCESS_NAME_MAX];
        snprintf(name, sizeof(name), "http-worker-%u", (unsigned)index);
        const process_create_params_t params = {
            .name = name,
            .built_in = true,
            .start_in_background = true,
            .stack_bytes = HTTP_SERVER__WORKER_STACK_SIZE,
            .priority = HTTP_SERVER__WORKER_PRIORITY,
            .process_entry = http_server__worker,
            .process_entry_context = (void *)(uintptr_t)index,
        };
        bruce_process_id_t process_id = BRUCE_PROCESS_ID_INVALID;
        if (process_registry__create(&params, &process_id) != BRUCE_OK) {
            http_server__abort_idle_workers();
            return BRUCE_ERR_NO_MEMORY;
        }
        s_worker_process_ids[index] = process_id;
        ++s_workers_remaining;
    }
    return BRUCE_OK;
}

static esp_err_t http_server__route_handler(httpd_req_t *request) {
    const http_server__route_t *route = request->user_ctx;
    if (route->callback != NULL) {
        http_server__lock();
        if (!s_workers_accepting || s_worker_capacity == NULL ||
            xSemaphoreTake(s_worker_capacity, 0) != pdTRUE) {
            http_server__unlock();
            return http_server__send_unavailable(request);
        }

        httpd_req_t *async_request = NULL;
        esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
        if (err != ESP_OK) {
            xSemaphoreGive(s_worker_capacity);
            http_server__unlock();
            ESP_LOGE(TAG, "async request allocation failed: %s", esp_err_to_name(err));
            return http_server__send_unavailable(request);
        }
        const http_server__job_t job = {
            .request = async_request,
            .route = route,
        };
        if (xQueueSend(s_worker_queue, &job, 0) != pdTRUE) {
            xSemaphoreGive(s_worker_capacity);
            http_server__send_unavailable(async_request);
            httpd_req_async_handler_complete(async_request);
            http_server__unlock();
            return ESP_OK;
        }
        http_server__unlock();
        return ESP_OK;
    }
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
        bool dynamic = route->callback != NULL;
        if (route->method < BRUCE_HTTP_SERVER_GET || route->method > BRUCE_HTTP_SERVER_DELETE ||
            route->uri == NULL || route->uri[0] != '/' ||
            strnlen(route->uri, HTTP_SERVER__URI_MAX_LEN) >= HTTP_SERVER__URI_MAX_LEN ||
            (!dynamic && (route->content_type == NULL || route->content_type[0] == '\0')) ||
            (!dynamic && strnlen(route->content_type, HTTP_SERVER__CONTENT_TYPE_MAX_LEN) >=
                             HTTP_SERVER__CONTENT_TYPE_MAX_LEN) ||
            (route->status_code != 0 && (route->status_code < 100 || route->status_code > 599)) ||
            (route->body_len > 0 && route->body == NULL) ||
            (dynamic && (route->content_type != NULL || route->body != NULL || route->body_len != 0 ||
                         route->status_code != 0)) ||
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
    s_workers_accepting = false;
    s_routes = calloc(options->route_count, sizeof(*s_routes));
    if (s_routes == NULL) {
        s_transitioning = false;
        http_server__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    for (size_t index = 0; index < options->route_count; ++index) {
        const bruce_http_server_route_t *source = &options->routes[index];
        http_server__route_t *target = &s_routes[index];
        snprintf(target->uri, sizeof(target->uri), "%s", source->uri);
        if (source->content_type != NULL) {
            snprintf(target->content_type, sizeof(target->content_type), "%s", source->content_type);
        }
        target->body_len = source->body_len;
        target->method = source->method;
        target->callback = source->callback;
        target->context = source->context;
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
    result = http_server__start_workers();
    if (result != BRUCE_OK) {
        http_server__free_routes();
        s_transitioning = false;
        http_server__unlock();
        return result;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = BRUCE_HTTP_SERVER_MAX_ROUTES;
    config.max_open_sockets = HTTP_SERVER__WORKER_COUNT + HTTP_SERVER__WORKER_QUEUE_LENGTH + 2u;
    config.lru_purge_enable = true;
    for (size_t index = 0; index < s_route_count; ++index) {
        if (strpbrk(s_routes[index].uri, "*?") != NULL) {
            config.uri_match_fn = httpd_uri_match_wildcard;
            break;
        }
    }
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server start failed: %s", esp_err_to_name(err));
        http_server__abort_idle_workers();
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
            .method = http_server__method(s_routes[index].method),
            .handler = http_server__route_handler,
            .user_ctx = &s_routes[index],
        };
        err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "route registration failed: %s", esp_err_to_name(err));
        http_server__abort_idle_workers();
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
    s_workers_accepting = true;
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
    if (server != NULL) {
        s_transitioning = true;
        s_workers_accepting = false;
        atomic_store(&s_workers_stopping, true);
    }
    http_server__unlock();
    if (server == NULL) return BRUCE_OK;

    result = http_server__wait_for_workers(pdMS_TO_TICKS(HTTP_SERVER__WORKER_STOP_TIMEOUT_MS));
    if (result != BRUCE_OK) {
        http_server__lock();
        s_transitioning = false;
        http_server__unlock();
        return result;
    }

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
