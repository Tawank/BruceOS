/* Background HTTP: requests queued by http__request_start() run on a small
 * pool of Core worker tasks. Each worker keeps its connection open between
 * requests, so a burst to one host (map tiles, API paging) pays for the
 * TCP/TLS handshake once. Workers never call back into the app: the body is
 * buffered here and handed over by http__request_poll() on the caller's task,
 * where its permissions and memory ownership apply. */

#include "core_sdk/http.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/http/http.h"
#include "core/network/network.h"
#include "core/process/process.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HTTP_ASYNC__JOBS_MAX 32
#define HTTP_ASYNC__INDEX_BITS 5
#define HTTP_ASYNC__WORKERS_PSRAM 4
#define HTTP_ASYNC__WORKERS_INTERNAL 1
#define HTTP_ASYNC__WORKER_STACK 8192
#define HTTP_ASYNC__WORKER_PRIORITY (tskIDLE_PRIORITY + 2)
#define HTTP_ASYNC__KEEP_ALIVE_MS 10000
#define HTTP_ASYNC__WORKER_EXIT_MS 60000
#define HTTP_ASYNC__DEFAULT_TIMEOUT_MS 30000
#define HTTP_ASYNC__MAX_REDIRECTS 5
#define HTTP_ASYNC__URL_MAX 512
#define HTTP_ASYNC__ORIGIN_MAX 128
#define HTTP_ASYNC__HEADER_NAME_MAX 48
#define HTTP_ASYNC__MIN_BUFFER 4096

typedef enum {
    HTTP_ASYNC__FREE,
    HTTP_ASYNC__QUEUED,
    HTTP_ASYNC__RUNNING,
    HTTP_ASYNC__DONE,
} http_async__state_t;

typedef struct {
    http_async__state_t state;
    bruce_http_job_id_t id;
    bool cancelled;
    bruce_process_id_t owner;
    bruce_resource_id_t resource_id;

    char *copy; /* Owns url, method, headers and body below. */
    const char *url;
    const char *method;
    const char *body;
    size_t body_len;
    const char *headers[2 * BRUCE_HTTP_ASYNC_MAX_HEADERS];
    size_t header_count;
    uint32_t timeout_ms;
    size_t max_response_bytes;

    bruce_result_t result;
    int status_code;
    uint8_t *response;
    size_t response_len;
    size_t response_capacity;
} http_async__job_t;

typedef struct {
    esp_http_client_handle_t client;
    char origin[HTTP_ASYNC__ORIGIN_MAX];
    char header_names[BRUCE_HTTP_ASYNC_MAX_HEADERS][HTTP_ASYNC__HEADER_NAME_MAX];
    size_t header_count;
    http_async__job_t *job;
    char location[HTTP_ASYNC__URL_MAX];
    char redirect_url[HTTP_ASYNC__URL_MAX];
} http_async__worker_t;

static const char *const TAG = "bruce_http_async";

static http_async__job_t s_jobs[HTTP_ASYNC__JOBS_MAX];
static uint32_t s_generation;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static size_t s_workers_alive;
static size_t s_workers_idle;
static size_t s_workers_max;
static uint32_t s_buffer_caps;

/* ---- job table ---------------------------------------------------------- */

static bool http_async__init(void) {
    if (s_queue != NULL) return true;
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    portEXIT_CRITICAL(&s_init_mux);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_queue == NULL) {
        bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
        s_workers_max = psram ? HTTP_ASYNC__WORKERS_PSRAM : HTTP_ASYNC__WORKERS_INTERNAL;
        s_buffer_caps = psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;
        s_queue = xQueueCreate(HTTP_ASYNC__JOBS_MAX, sizeof(bruce_http_job_id_t));
    }
    xSemaphoreGive(s_lock);
    return s_queue != NULL;
}

static void http_async__lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void http_async__unlock(void) { xSemaphoreGive(s_lock); }

static http_async__job_t *http_async__find_locked(bruce_http_job_id_t id) {
    http_async__job_t *job = &s_jobs[id & (HTTP_ASYNC__JOBS_MAX - 1)];
    return job->state != HTTP_ASYNC__FREE && job->id == id ? job : NULL;
}

static http_async__job_t *http_async__find_owned_locked(bruce_http_job_id_t id) {
    http_async__job_t *job = http_async__find_locked(id);
    return job != NULL && job->owner == process__current_id() ? job : NULL;
}

/* Read by a worker mid-transfer without the lock, set by cancel under it. */
static bool http_async__is_cancelled(const http_async__job_t *job) {
    return __atomic_load_n(&job->cancelled, __ATOMIC_RELAXED);
}

static void http_async__release_locked(http_async__job_t *job) {
    free(job->copy);
    heap_caps_free(job->response);
    memset(job, 0, sizeof(*job));
}

static size_t http_async__count_owned_locked(bruce_process_id_t owner) {
    size_t count = 0;
    for (size_t i = 0; i < HTTP_ASYNC__JOBS_MAX; ++i) {
        count += s_jobs[i].state != HTTP_ASYNC__FREE && !s_jobs[i].cancelled && s_jobs[i].owner == owner;
    }
    return count;
}

static http_async__job_t *http_async__alloc_locked(void) {
    for (size_t i = 0; i < HTTP_ASYNC__JOBS_MAX; ++i) {
        http_async__job_t *job = &s_jobs[i];
        if (job->state != HTTP_ASYNC__FREE) continue;
        if (++s_generation == 0) s_generation = 1;
        job->id = (s_generation << HTTP_ASYNC__INDEX_BITS) | (bruce_http_job_id_t)i;
        job->state = HTTP_ASYNC__QUEUED;
        return job;
    }
    return NULL;
}

/* One block holding every string the worker needs after the caller returns. */
static bool http_async__copy_request(http_async__job_t *job, const bruce_http_request_t *request) {
    size_t size = strlen(request->url) + 1 + (request->method != NULL ? strlen(request->method) + 1 : 0) +
                  request->body_len;
    for (size_t i = 0; i < 2 * request->header_count; ++i) {
        size += request->headers[i] != NULL ? strlen(request->headers[i]) + 1 : 1;
    }
    char *cursor = malloc(size);
    if (cursor == NULL) return false;
    job->copy = cursor;

#define HTTP_ASYNC__COPY(dst, src)          \
    do {                                    \
        size_t len = strlen(src) + 1;       \
        memcpy(cursor, (src), len);         \
        (dst) = cursor;                     \
        cursor += len;                      \
    } while (0)

    HTTP_ASYNC__COPY(job->url, request->url);
    if (request->method != NULL) HTTP_ASYNC__COPY(job->method, request->method);
    for (size_t i = 0; i < 2 * request->header_count; ++i) {
        HTTP_ASYNC__COPY(job->headers[i], request->headers[i] != NULL ? request->headers[i] : "");
    }
#undef HTTP_ASYNC__COPY
    job->header_count = request->header_count;
    if (request->body != NULL && request->body_len > 0) {
        memcpy(cursor, request->body, request->body_len);
        job->body = cursor;
        job->body_len = request->body_len;
    }
    return true;
}

/* ---- worker ------------------------------------------------------------- */

static bool http_async__reserve(http_async__job_t *job, size_t needed) {
    if (needed <= job->response_capacity) return true;
    size_t capacity = job->response_capacity > 0 ? job->response_capacity * 2 : HTTP_ASYNC__MIN_BUFFER;
    if (capacity < needed) capacity = needed;
    if (capacity > job->max_response_bytes) capacity = job->max_response_bytes;
    uint8_t *grown = heap_caps_realloc(job->response, capacity, s_buffer_caps);
    if (grown == NULL) grown = heap_caps_realloc(job->response, capacity, MALLOC_CAP_8BIT);
    if (grown == NULL) return false;
    job->response = grown;
    job->response_capacity = capacity;
    return true;
}

static void http_async__append(http_async__job_t *job, const void *data, size_t len) {
    if (job->result != BRUCE_OK || http_async__is_cancelled(job)) return;
    if (len > job->max_response_bytes - job->response_len) {
        job->result = BRUCE_ERR_RESOURCE_LIMIT;
        return;
    }
    if (!http_async__reserve(job, job->response_len + len)) {
        job->result = BRUCE_ERR_NO_MEMORY;
        return;
    }
    memcpy(job->response + job->response_len, data, len);
    job->response_len += len;
}

static esp_err_t http_async__event(esp_http_client_event_t *evt) {
    http_async__worker_t *worker = evt->user_data;
    if (worker == NULL || worker->job == NULL) return ESP_OK;
    http_async__job_t *job = worker->job;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL && evt->header_value != NULL) {
        size_t length = 0;
        if (strcasecmp(evt->header_key, "Location") == 0) {
            snprintf(worker->location, sizeof(worker->location), "%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "Content-Length") == 0 &&
                   http__parse_content_length(evt->header_value, &length)) {
            if (length <= job->max_response_bytes) (void)http_async__reserve(job, length);
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int status = esp_http_client_get_status_code(evt->client);
        bool redirect = http__is_redirect_status(status) && worker->location[0] != '\0';
        if (!redirect) http_async__append(job, evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

static void http_async__close(http_async__worker_t *worker) {
    if (worker->client == NULL) return;
    esp_http_client_cleanup(worker->client);
    worker->client = NULL;
    worker->origin[0] = '\0';
    worker->header_count = 0;
}

/* "https://host:port" part of a URL, the unit a kept-alive connection serves. */
static void http_async__origin(const char *url, char *out, size_t capacity) {
    const char *host = strstr(url, "://");
    const char *end = host != NULL ? strchr(host + 3, '/') : NULL;
    size_t length = end != NULL ? (size_t)(end - url) : strlen(url);
    if (length >= capacity) length = capacity - 1;
    memcpy(out, url, length);
    out[length] = '\0';
}

static bool http_async__open(http_async__worker_t *worker, const char *url) {
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HTTP_ASYNC__DEFAULT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = http_async__event,
        .user_data = worker,
    };
    worker->client = esp_http_client_init(&config);
    if (worker->client == NULL) return false;
    http_async__origin(url, worker->origin, sizeof(worker->origin));
    return true;
}

static void http_async__prepare(http_async__worker_t *worker, const char *method, bool send_body) {
    http_async__job_t *job = worker->job;
    esp_http_client_handle_t client = worker->client;
    for (size_t i = 0; i < worker->header_count; ++i) {
        esp_http_client_delete_header(client, worker->header_names[i]);
    }
    worker->header_count = 0;
    for (size_t i = 0; i < job->header_count; ++i) {
        const char *name = job->headers[2 * i];
        if (name[0] == '\0' || strlen(name) >= HTTP_ASYNC__HEADER_NAME_MAX) continue;
        esp_http_client_set_header(client, name, job->headers[2 * i + 1]);
        snprintf(worker->header_names[worker->header_count++], HTTP_ASYNC__HEADER_NAME_MAX, "%s", name);
    }
    esp_http_client_set_method(client, http__method_from_string(method));
    esp_http_client_set_timeout_ms(client, job->timeout_ms);
    bool has_body = send_body && job->body != NULL;
    esp_http_client_set_post_field(client, has_body ? job->body : NULL, has_body ? (int)job->body_len : 0);
}

/* One request, on the kept-alive connection when it serves the same origin.
 * A kept connection the server already dropped fails fast, so that case gets
 * one retry on a fresh connection. */
static esp_err_t
http_async__perform(http_async__worker_t *worker, const char *url, const char *method, bool send_body) {
    char origin[HTTP_ASYNC__ORIGIN_MAX];
    http_async__origin(url, origin, sizeof(origin));
    bool reused = worker->client != NULL && strcmp(origin, worker->origin) == 0;
    for (;;) {
        if (!reused) {
            http_async__close(worker);
            if (!http_async__open(worker, url)) return ESP_ERR_NO_MEM;
        } else if (esp_http_client_set_url(worker->client, url) != ESP_OK) {
            http_async__close(worker);
            return ESP_FAIL;
        }
        http_async__prepare(worker, method, send_body);
        worker->job->response_len = 0;
        worker->job->result = BRUCE_OK;
        worker->location[0] = '\0';

        esp_err_t err = esp_http_client_perform(worker->client);
        if (err == ESP_OK) {
            worker->job->status_code = esp_http_client_get_status_code(worker->client);
            return ESP_OK;
        }
        http_async__close(worker);
        if (!reused) return err;
        reused = false;
    }
}

static void http_async__run(http_async__worker_t *worker, http_async__job_t *job) {
    worker->job = job;
    const char *url = job->url;
    const char *method = job->method;
    bool send_body = true;
    for (int redirects = 0;; ++redirects) {
        esp_err_t err = http_async__perform(worker, url, method, send_body);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "request failed for %s: %s", url, esp_err_to_name(err));
            if (job->result == BRUCE_OK) job->result = BRUCE_ERR_IO;
            break;
        }
        bool follow = http__is_redirect_status(job->status_code) && worker->location[0] != '\0' &&
                      strstr(worker->location, "://") != NULL && redirects < HTTP_ASYNC__MAX_REDIRECTS;
        if (!follow || job->result != BRUCE_OK) break;
        snprintf(worker->redirect_url, sizeof(worker->redirect_url), "%s", worker->location);
        url = worker->redirect_url;
        if (job->status_code != 307 && job->status_code != 308) {
            if (method == NULL || strcasecmp(method, "HEAD") != 0) method = "GET";
            send_body = false;
        }
    }
    worker->job = NULL;
}

static void http_async__worker_task(void *arg) {
    (void)arg;
    http_async__worker_t *worker = calloc(1, sizeof(*worker));
    for (;;) {
        bruce_http_job_id_t id = 0;
        http_async__lock();
        s_workers_idle++;
        http_async__unlock();
        uint32_t wait_ms = worker != NULL && worker->client != NULL ? HTTP_ASYNC__KEEP_ALIVE_MS
                                                                    : HTTP_ASYNC__WORKER_EXIT_MS;
        bool received = worker != NULL && xQueueReceive(s_queue, &id, pdMS_TO_TICKS(wait_ms)) == pdTRUE;

        http_async__lock();
        s_workers_idle--;
        if (!received) {
            if (worker != NULL && worker->client != NULL) {
                http_async__unlock();
                http_async__close(worker);
                continue;
            }
            if (worker != NULL && uxQueueMessagesWaiting(s_queue) > 0) {
                http_async__unlock();
                continue;
            }
            s_workers_alive--;
            http_async__unlock();
            free(worker);
            vTaskDelete(NULL);
            return;
        }
        http_async__job_t *job = http_async__find_locked(id);
        if (job == NULL || job->state != HTTP_ASYNC__QUEUED) {
            http_async__unlock();
            continue;
        }
        job->state = HTTP_ASYNC__RUNNING;
        http_async__unlock();

        http_async__run(worker, job);

        http_async__lock();
        if (job->cancelled) http_async__release_locked(job);
        else job->state = HTTP_ASYNC__DONE;
        http_async__unlock();
    }
}

/* Adds a worker while queued jobs outnumber idle ones, up to the board's limit. */
static void http_async__ensure_worker(void) {
    http_async__lock();
    bool spawn = uxQueueMessagesWaiting(s_queue) > s_workers_idle && s_workers_alive < s_workers_max;
    if (spawn) s_workers_alive++;
    http_async__unlock();
    if (!spawn) return;
    BaseType_t created = xTaskCreate(
        http_async__worker_task, "http_async", HTTP_ASYNC__WORKER_STACK, NULL, HTTP_ASYNC__WORKER_PRIORITY,
        NULL
    );
    if (created != pdPASS) {
        http_async__lock();
        s_workers_alive--;
        http_async__unlock();
    }
}

/* ---- public API --------------------------------------------------------- */

static void http_async__cancel_locked(http_async__job_t *job) {
    if (job->state == HTTP_ASYNC__RUNNING) __atomic_store_n(&job->cancelled, true, __ATOMIC_RELAXED);
    else http_async__release_locked(job);
}

/* Process exit: the registry drops the resource itself. */
static void http_async__cleanup(void *context) {
    bruce_http_job_id_t id = (bruce_http_job_id_t)(uintptr_t)context;
    http_async__lock();
    http_async__job_t *job = http_async__find_locked(id);
    if (job != NULL) http_async__cancel_locked(job);
    http_async__unlock();
}

bruce_result_t http__request_start(const bruce_http_request_t *request, bruce_http_job_id_t *out_job) {
    if (out_job == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_job = BRUCE_HTTP_JOB_ID_INVALID;
    if (request == NULL || request->url == NULL || request->on_response_chunk != NULL ||
        request->header_count > BRUCE_HTTP_ASYNC_MAX_HEADERS ||
        (request->header_count > 0 && request->headers == NULL) || request->body_len > INT_MAX ||
        strlen(request->url) >= HTTP_ASYNC__URL_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_result_t result = permission__check(BRUCE_PERMISSION_HTTP);
    if (result != BRUCE_OK) return result;
    result = network__init();
    if (result != BRUCE_OK) return result;
    if (!http_async__init()) return BRUCE_ERR_NO_MEMORY;

    bruce_process_id_t owner = process__current_id();
    http_async__lock();
    bool below_limit = http_async__count_owned_locked(owner) < BRUCE_HTTP_ASYNC_MAX_JOBS;
    http_async__job_t *job = below_limit ? http_async__alloc_locked() : NULL;
    if (job == NULL) {
        http_async__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    job->owner = owner;
    job->timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : HTTP_ASYNC__DEFAULT_TIMEOUT_MS;
    job->max_response_bytes =
        request->max_response_bytes > 0 ? request->max_response_bytes : BRUCE_HTTP_DEFAULT_MAX_RESPONSE_BYTES;
    bool copied = http_async__copy_request(job, request);
    bruce_http_job_id_t id = job->id;
    if (!copied) http_async__release_locked(job);
    http_async__unlock();
    if (!copied) return BRUCE_ERR_NO_MEMORY;

    bruce_resource_id_t resource_id =
        process_registry__resource_register(http_async__cleanup, (void *)(uintptr_t)id);
    http_async__lock();
    job = http_async__find_locked(id);
    if (resource_id == BRUCE_RESOURCE_ID_INVALID) {
        if (job != NULL) http_async__release_locked(job);
        http_async__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    job->resource_id = resource_id;
    http_async__unlock();

    (void)xQueueSend(s_queue, &id, 0);
    http_async__ensure_worker();
    *out_job = id;
    return BRUCE_OK;
}

bruce_result_t http__request_poll(bruce_http_job_id_t id, bruce_http_response_t *response) {
    if (response == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(response, 0, sizeof(*response));
    if (s_lock == NULL) return BRUCE_ERR_NOT_FOUND;

    http_async__lock();
    http_async__job_t *job = http_async__find_owned_locked(id);
    if (job == NULL || job->cancelled) {
        http_async__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (job->state != HTTP_ASYNC__DONE) {
        http_async__unlock();
        return BRUCE_ERR_BUSY;
    }
    bruce_result_t result = job->result;
    int status_code = job->status_code;
    uint8_t *body = job->response;
    size_t body_len = job->response_len;
    bruce_resource_id_t resource_id = job->resource_id;
    job->response = NULL;
    http_async__release_locked(job);
    http_async__unlock();
    (void)process_registry__resource_release(resource_id);

    if (result == BRUCE_OK) {
        /* Copied into the caller's own memory: PSRAM when fitted, never flash swap. */
        char *owned = (char *)memory__external_malloc_writable(body_len + 1);
        if (owned != NULL) {
            if (body_len > 0) memcpy(owned, body, body_len);
            owned[body_len] = '\0';
            response->status_code = status_code;
            response->body = owned;
            response->body_len = body_len;
            response->body_object = owned;
        } else {
            result = BRUCE_ERR_NO_MEMORY;
        }
    }
    heap_caps_free(body);
    return result;
}

void http__request_cancel(bruce_http_job_id_t id) {
    if (s_lock == NULL) return;
    http_async__lock();
    http_async__job_t *job = http_async__find_owned_locked(id);
    if (job == NULL || job->cancelled) {
        http_async__unlock();
        return;
    }
    bruce_resource_id_t resource_id = job->resource_id;
    http_async__cancel_locked(job);
    http_async__unlock();
    (void)process_registry__resource_release(resource_id);
}
