#include "stdio.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/task/task.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"

#define STDIO__MAX_SESSIONS 4
#define STDIO__OUTPUT_CAPACITY 1024
#define STDIO__INPUT_CAPACITY 256

typedef struct {
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
static stdio__session_t *s_sessions[STDIO__MAX_SESSIONS];
static bruce_stdio_session_t s_next_id = 1;
#if !CONFIG_LIBC_PICOLIBC
static bool s_original_stdio_saved = false;
static FILE *s_original_stdin = NULL;
static FILE *s_original_stdout = NULL;
static FILE *s_original_stderr = NULL;

#if CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
extern __thread FILE *tls_stdin;
extern __thread FILE *tls_stdout;
extern __thread FILE *tls_stderr;
static FILE *s_original_tls_stdin = NULL;
static FILE *s_original_tls_stdout = NULL;
static FILE *s_original_tls_stderr = NULL;
#endif
#endif

static void stdio__ensure_init(void) {
    if (s_lock != NULL) return;
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    portEXIT_CRITICAL(&s_init_mux);
}

static stdio__session_t *stdio__find_locked(bruce_stdio_session_t id) {
    for (size_t i = 0; i < STDIO__MAX_SESSIONS; ++i) {
        if (s_sessions[i] != NULL && s_sessions[i]->id == id) return s_sessions[i];
    }
    return NULL;
}

static void stdio__remove_locked(stdio__session_t *session) {
    for (size_t i = 0; i < STDIO__MAX_SESSIONS; ++i) {
        if (s_sessions[i] == session) {
            s_sessions[i] = NULL;
            return;
        }
    }
}

static bool stdio__owned_locked(const stdio__session_t *session, bruce_task_id_t owner) {
    return session != NULL && session->owner == owner;
}

static void stdio__session_cleanup(void *context) {
    stdio__session_t *session = context;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__remove_locked(session);
    xSemaphoreGive(s_lock);
    free(session);
}

bruce_result_t bruce_stdio_session_create(bruce_stdio_session_t *out_session) {
    if (out_session == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_session = BRUCE_STDIO_SESSION_INVALID;
    bruce_task_id_t owner = task__current_id();
    stdio__session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) return BRUCE_ERR_NO_MEMORY;

    session->owner = owner;
    session->resource = task_registry__resource_register(stdio__session_cleanup, session);
    if (session->resource == BRUCE_RESOURCE_ID_INVALID) {
        free(session);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t slot = STDIO__MAX_SESSIONS;
    for (size_t i = 0; i < STDIO__MAX_SESSIONS; ++i) {
        if (s_sessions[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == STDIO__MAX_SESSIONS) {
        xSemaphoreGive(s_lock);
        (void)task_registry__resource_release(session->resource);
        free(session);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    session->id = s_next_id++;
    if (s_next_id == BRUCE_STDIO_SESSION_INVALID) s_next_id++;
    s_sessions[slot] = session;
    xSemaphoreGive(s_lock);
    *out_session = session->id;
    return BRUCE_OK;
}

bruce_result_t bruce_stdio_session_close(bruce_stdio_session_t session) {
    bruce_task_id_t owner = task__current_id();
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (!stdio__owned_locked(entry, owner)) {
        xSemaphoreGive(s_lock);
        return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    bruce_resource_id_t resource = entry->resource;
    stdio__remove_locked(entry);
    xSemaphoreGive(s_lock);
    (void)task_registry__resource_release(resource);
    free(entry);
    (void)task_registry__set_child_stdio_session(BRUCE_STDIO_SESSION_INVALID);
    return BRUCE_OK;
}

bruce_result_t bruce_stdio_session_route_children(bruce_stdio_session_t session) {
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

bruce_result_t bruce_stdio_session_write_input(bruce_stdio_session_t session, const void *data, size_t size) {
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

bruce_result_t bruce_stdio_session_read_output(
    bruce_stdio_session_t session, void *buffer, size_t capacity, size_t *out_size
) {
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

bruce_result_t stdio__session_read_input(
    bruce_stdio_session_t session, void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size
) {
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

#if !CONFIG_LIBC_PICOLIBC
static int stdio__stream_read(void *cookie, char *buffer, int size) {
    size_t read_size = 0;
    bruce_result_t result = stdio__session_read_input(
        (bruce_stdio_session_t)(uintptr_t)cookie, buffer, (size_t)size, UINT32_MAX, &read_size
    );
    return result == BRUCE_OK ? (int)read_size : 0;
}
#endif

static bruce_result_t stdio__session_write_output(
    bruce_stdio_session_t session, const void *data, size_t size
) {
    if (data == NULL || size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_NOT_FOUND;
    }
    const char *buffer = data;
    for (size_t i = 0; i < size; ++i) {
        if (entry->output_size == STDIO__OUTPUT_CAPACITY) {
            entry->output_read = (entry->output_read + 1) % STDIO__OUTPUT_CAPACITY;
            entry->output_size--;
        }
        size_t write_at = (entry->output_read + entry->output_size) % STDIO__OUTPUT_CAPACITY;
        entry->output[write_at] = buffer[i];
        entry->output_size++;
    }
    xSemaphoreGive(s_lock);
    return BRUCE_OK;
}

#if !CONFIG_LIBC_PICOLIBC
static int stdio__stream_write(void *cookie, const char *buffer, int size) {
    if (size <= 0) return 0;
    bruce_result_t result = stdio__session_write_output(
        (bruce_stdio_session_t)(uintptr_t)cookie, buffer, (size_t)size
    );
    return result == BRUCE_OK ? size : 0;
}
#endif

void stdio__task_attach(
    bruce_stdio_session_t session, FILE **out_input, FILE **out_output, FILE **out_error
) {
    if (out_input != NULL) *out_input = NULL;
    if (out_output != NULL) *out_output = NULL;
    if (out_error != NULL) *out_error = NULL;
#if CONFIG_LIBC_PICOLIBC
    (void)session;
#else
    if (session == BRUCE_STDIO_SESSION_INVALID) return;
    FILE *input = funopen((void *)(uintptr_t)session, stdio__stream_read, NULL, NULL, NULL);
    FILE *output = funopen((void *)(uintptr_t)session, NULL, stdio__stream_write, NULL, NULL);
    FILE *error = funopen((void *)(uintptr_t)session, NULL, stdio__stream_write, NULL, NULL);
    if (input == NULL || output == NULL || error == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        if (error != NULL) fclose(error);
        return;
    }
    setvbuf(output, NULL, _IONBF, 0);
    setvbuf(error, NULL, _IONBF, 0);
    if (!s_original_stdio_saved) {
        s_original_stdin = stdin;
        s_original_stdout = stdout;
        s_original_stderr = stderr;
#if CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
        s_original_tls_stdin = tls_stdin;
        s_original_tls_stdout = tls_stdout;
        s_original_tls_stderr = tls_stderr;
#endif
        s_original_stdio_saved = true;
    }
    stdin = input;
    stdout = output;
    stderr = error;
#if CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
    tls_stdin = input;
    tls_stdout = output;
    tls_stderr = error;
#endif
    if (out_input != NULL) *out_input = input;
    if (out_output != NULL) *out_output = output;
    if (out_error != NULL) *out_error = error;
#endif
}

void stdio__task_detach(FILE *input, FILE *output, FILE *error) {
#if CONFIG_LIBC_PICOLIBC
    (void)input;
    (void)output;
    (void)error;
#else
    if (input != NULL) {
        if (stdin == input) stdin = s_original_stdin;
#if CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
        if (tls_stdin == input) tls_stdin = s_original_tls_stdin;
#endif
        fclose(input);
    }
    if (output != NULL) {
        if (stdout == output) stdout = s_original_stdout;
#if CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
        if (tls_stdout == output) tls_stdout = s_original_tls_stdout;
#endif
        fclose(output);
    }
    if (error != NULL) {
        if (stderr == error) stderr = s_original_stderr;
#if CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
        if (tls_stderr == error) tls_stderr = s_original_tls_stderr;
#endif
        fclose(error);
    }
#endif
}

bruce_result_t bruce_stdio_write(const void *data, size_t size) {
    if (size == 0) return BRUCE_OK;
    if (data == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_stdio_session_t session = task_registry__current_stdio_session();
    if (session != BRUCE_STDIO_SESSION_INVALID) {
        return stdio__session_write_output(session, data, size);
    }

    const char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(STDOUT_FILENO, bytes + offset, size - offset);
        if (written <= 0) return BRUCE_ERR_IO;
        offset += (size_t)written;
    }
    return BRUCE_OK;
}

int stdio__vprintf(const char *format, va_list args) {
    if (format == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    char stack_buffer[256];
    va_list measure;
    va_copy(measure, args);
    int size = vsnprintf(stack_buffer, sizeof(stack_buffer), format, measure);
    va_end(measure);
    if (size < 0) return BRUCE_ERR_IO;

    char *output = stack_buffer;
    if ((size_t)size >= sizeof(stack_buffer)) {
        output = malloc((size_t)size + 1);
        if (output == NULL) return BRUCE_ERR_NO_MEMORY;
        (void)vsnprintf(output, (size_t)size + 1, format, args);
    }
    bruce_result_t result = bruce_stdio_write(output, (size_t)size);
    if (output != stack_buffer) free(output);
    return result == BRUCE_OK ? size : (int)result;
}

int stdio__printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = stdio__vprintf(format, args);
    va_end(args);
    return result;
}

bruce_result_t bruce_stdio_read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size) {
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

int bruce_stdio_read_line(char *buffer, size_t buffer_size, bool mask_input) {
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
            int ch = getchar();
            if (ch == EOF) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            c = ch;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (c == '\b' || c == 0x7f) {
            if (i > 0) {
                i--;
                if (!mask_input) {
                    (void)bruce_stdio_write("\b \b", 3);
                }
            }
            continue;
        }
        buffer[i++] = (char)c;
        if (!mask_input) {
            char byte = (char)c;
            (void)bruce_stdio_write(&byte, 1);
        }
    }
    buffer[i] = '\0';
    (void)bruce_stdio_write("\n", 1);
    return eof && i == 0 ? -1 : (int)i;
}
