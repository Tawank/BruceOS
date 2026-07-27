#include "stdio.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/task/task.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"

#define STDIO__MAX_SESSIONS 4
#define STDIO__OUTPUT_CAPACITY 4096
#define STDIO__INPUT_CAPACITY 512

typedef struct {
    bool active;
    bruce_stdio_session_t id;
    bruce_task_id_t owner;
    bruce_resource_id_t resource;
    char output[STDIO__OUTPUT_CAPACITY];
    size_t output_read;
    size_t output_size;
    char input[STDIO__INPUT_CAPACITY];
    size_t input_read;
    size_t input_size;
} stdio__session_t;

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static stdio__session_t s_sessions[STDIO__MAX_SESSIONS];
static bruce_stdio_session_t s_next_id = 1;

static void stdio__ensure_init(void)
{
    if (s_lock != NULL) return;
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    portEXIT_CRITICAL(&s_init_mux);
}

static stdio__session_t *stdio__find_locked(bruce_stdio_session_t id)
{
    for (size_t i = 0; i < STDIO__MAX_SESSIONS; ++i) {
        if (s_sessions[i].active && s_sessions[i].id == id) return &s_sessions[i];
    }
    return NULL;
}

static bool stdio__owned_locked(const stdio__session_t *session, bruce_task_id_t owner)
{
    return session != NULL && session->owner == owner;
}

static void stdio__session_cleanup(void *context)
{
    bruce_stdio_session_t id = (bruce_stdio_session_t)(uintptr_t)context;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(id);
    if (entry != NULL) memset(entry, 0, sizeof(*entry));
    xSemaphoreGive(s_lock);
}

bruce_result_t bruce_stdio_session_create(bruce_stdio_session_t *out_session)
{
    if (out_session == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_session = BRUCE_STDIO_SESSION_INVALID;
    bruce_task_id_t owner = task__current_id();
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < STDIO__MAX_SESSIONS; ++i) {
        if (!s_sessions[i].active) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            s_sessions[i].active = true;
            s_sessions[i].id = s_next_id++;
            if (s_next_id == BRUCE_STDIO_SESSION_INVALID) s_next_id++;
            s_sessions[i].owner = owner;
            bruce_stdio_session_t id = s_sessions[i].id;
            xSemaphoreGive(s_lock);
            bruce_resource_id_t resource = task_registry__resource_register(
                stdio__session_cleanup, (void *)(uintptr_t)id);
            if (resource == BRUCE_RESOURCE_ID_INVALID) {
                stdio__session_cleanup((void *)(uintptr_t)id);
                return BRUCE_ERR_RESOURCE_LIMIT;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            stdio__session_t *entry = stdio__find_locked(id);
            if (entry != NULL) entry->resource = resource;
            xSemaphoreGive(s_lock);
            *out_session = id;
            return BRUCE_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return BRUCE_ERR_RESOURCE_LIMIT;
}

bruce_result_t bruce_stdio_session_close(bruce_stdio_session_t session)
{
    bruce_task_id_t owner = task__current_id();
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (!stdio__owned_locked(entry, owner)) {
        xSemaphoreGive(s_lock);
        return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    bruce_resource_id_t resource = entry->resource;
    memset(entry, 0, sizeof(*entry));
    xSemaphoreGive(s_lock);
    (void)task_registry__resource_release(resource);
    (void)task_registry__set_child_stdio_session(BRUCE_STDIO_SESSION_INVALID);
    return BRUCE_OK;
}

bruce_result_t bruce_stdio_session_route_children(bruce_stdio_session_t session)
{
    if (session != BRUCE_STDIO_SESSION_INVALID) {
        bruce_task_id_t owner = task__current_id();
        stdio__ensure_init();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        stdio__session_t *entry = stdio__find_locked(session);
        bool owned = stdio__owned_locked(entry, owner);
        xSemaphoreGive(s_lock);
        if (!owned) return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    return task_registry__set_child_stdio_session(session);
}

bruce_result_t bruce_stdio_session_write_input(bruce_stdio_session_t session, const void *data, size_t size)
{
    if (data == NULL || size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_task_id_t owner = task__current_id();
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (!stdio__owned_locked(entry, owner)) {
        xSemaphoreGive(s_lock);
        return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    if (size > STDIO__INPUT_CAPACITY - entry->input_size) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    const char *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        size_t write_at = (entry->input_read + entry->input_size) % STDIO__INPUT_CAPACITY;
        entry->input[write_at] = bytes[i];
        entry->input_size++;
    }
    xSemaphoreGive(s_lock);
    return BRUCE_OK;
}

bruce_result_t bruce_stdio_session_read_output(bruce_stdio_session_t session, void *buffer, size_t capacity,
                                                size_t *out_size)
{
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    bruce_task_id_t owner = task__current_id();
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (!stdio__owned_locked(entry, owner)) {
        xSemaphoreGive(s_lock);
        return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    size_t copied = capacity < entry->output_size ? capacity : entry->output_size;
    char *bytes = buffer;
    for (size_t i = 0; i < copied; ++i) {
        bytes[i] = entry->output[entry->output_read];
        entry->output_read = (entry->output_read + 1) % STDIO__OUTPUT_CAPACITY;
    }
    entry->output_size -= copied;
    *out_size = copied;
    xSemaphoreGive(s_lock);
    return copied > 0 ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
}

bruce_result_t stdio__session_read_input(bruce_stdio_session_t session, void *buffer, size_t capacity,
                                         uint32_t timeout_ms, size_t *out_size)
{
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    uint64_t started = runtime__now();
    for (;;) {
        stdio__ensure_init();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        stdio__session_t *entry = stdio__find_locked(session);
        if (entry == NULL) {
            xSemaphoreGive(s_lock);
            return BRUCE_ERR_NOT_FOUND;
        }
        size_t copied = capacity < entry->input_size ? capacity : entry->input_size;
        char *bytes = buffer;
        for (size_t i = 0; i < copied; ++i) {
            bytes[i] = entry->input[entry->input_read];
            entry->input_read = (entry->input_read + 1) % STDIO__INPUT_CAPACITY;
        }
        entry->input_size -= copied;
        xSemaphoreGive(s_lock);
        if (copied > 0) {
            *out_size = copied;
            return BRUCE_OK;
        }
        if (timeout_ms == 0 || runtime__now() - started >= timeout_ms) return BRUCE_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static int stdio__stream_read(void *cookie, char *buffer, int size)
{
    size_t read_size = 0;
    bruce_result_t result = stdio__session_read_input((bruce_stdio_session_t)(uintptr_t)cookie, buffer,
                                                       (size_t)size, UINT32_MAX, &read_size);
    return result == BRUCE_OK ? (int)read_size : 0;
}

static int stdio__stream_write(void *cookie, const char *buffer, int size)
{
    if (buffer == NULL || size <= 0) return 0;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked((bruce_stdio_session_t)(uintptr_t)cookie);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    for (int i = 0; i < size; ++i) {
        if (entry->output_size == STDIO__OUTPUT_CAPACITY) {
            entry->output_read = (entry->output_read + 1) % STDIO__OUTPUT_CAPACITY;
            entry->output_size--;
        }
        size_t write_at = (entry->output_read + entry->output_size) % STDIO__OUTPUT_CAPACITY;
        entry->output[write_at] = buffer[i];
        entry->output_size++;
    }
    xSemaphoreGive(s_lock);
    return size;
}

void stdio__task_attach(bruce_stdio_session_t session, FILE **out_input, FILE **out_output)
{
    if (out_input != NULL) *out_input = NULL;
    if (out_output != NULL) *out_output = NULL;
    if (session == BRUCE_STDIO_SESSION_INVALID) return;
    FILE *input = funopen((void *)(uintptr_t)session, stdio__stream_read, NULL, NULL, NULL);
    FILE *output = funopen((void *)(uintptr_t)session, NULL, stdio__stream_write, NULL, NULL);
    if (input == NULL || output == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        return;
    }
    setvbuf(output, NULL, _IONBF, 0);
    stdin = input;
    stdout = output;
    stderr = output;
    if (out_input != NULL) *out_input = input;
    if (out_output != NULL) *out_output = output;
}

void stdio__task_detach(FILE *input, FILE *output)
{
    if (output != NULL) fclose(output);
    if (input != NULL) fclose(input);
}

bruce_result_t bruce_stdio_read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size)
{
    bruce_stdio_session_t session = task_registry__current_stdio_session();
    if (session != BRUCE_STDIO_SESSION_INVALID) {
        return stdio__session_read_input(session, buffer, capacity, timeout_ms, out_size);
    }
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
    };
    int ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &timeout);
    if (ready == 0) return BRUCE_ERR_TIMEOUT;
    if (ready < 0) return BRUCE_ERR_IO;
    ssize_t count = read(STDIN_FILENO, buffer, capacity);
    if (count < 0) return BRUCE_ERR_IO;
    if (count == 0) return BRUCE_ERR_NOT_FOUND;
    *out_size = (size_t)count;
    return BRUCE_OK;
}

int bruce_stdio_read_line(char *buffer, size_t buffer_size, bool mask_input)
{
    if (buffer == NULL || buffer_size == 0) return -1;
    bruce_stdio_session_t session = task_registry__current_stdio_session();
    size_t i = 0;
    bool eof = false;
    while (i + 1 < buffer_size) {
        int c;
        if (session != BRUCE_STDIO_SESSION_INVALID) {
            char byte;
            size_t count = 0;
            bruce_result_t result = stdio__session_read_input(session, &byte, 1, 100, &count);
            if (result == BRUCE_ERR_TIMEOUT) continue;
            if (result != BRUCE_OK) {
                eof = true;
                break;
            }
            c = (unsigned char)byte;
        } else {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
            int ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
            c = getchar();
            if (c == EOF) {
                if (ready > 0) {
                    eof = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (c == '\b' || c == 0x7f) {
            if (i > 0) {
                i--;
                if (!mask_input) {
                    printf("\b \b");
                    fflush(stdout);
                }
            }
            continue;
        }
        buffer[i++] = (char)c;
        if (!mask_input) {
            putchar(c);
            fflush(stdout);
        }
    }
    buffer[i] = '\0';
    printf("\n");
    return eof && i == 0 ? -1 : (int)i;
}
