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

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#include "core/process/process.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/tty.h"

#define STDIO__MAX_SESSIONS 4
#define STDIO__OUTPUT_CAPACITY 1024
#define STDIO__INPUT_CAPACITY 256
#define STDIO__READ_POLL_MS 100u

typedef struct {
    bruce_stdio_session_t id;
    bruce_process_id_t owner;
    bruce_resource_id_t resource;
    char output[STDIO__OUTPUT_CAPACITY];
    size_t output_read;
    size_t output_size;
    char input[STDIO__INPUT_CAPACITY];
    size_t input_read;
    size_t input_size;
    uint16_t tty_columns;
    uint16_t tty_rows;
    uint32_t tty_generation; /* 0 == size never set (not a tty) */
    bruce_tty_mode_t tty_mode;
    bool output_last_was_cr; /* tracks '\r' across write() calls, for ONLCR translation */
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

bruce_result_t stdio__init(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .rx_buffer_size = 1024,
            .tx_buffer_size = 1024,
        };
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) return BRUCE_ERR_IO;
    }
    usb_serial_jtag_vfs_use_driver();
#endif
    return BRUCE_OK;
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

static bool stdio__owned_locked(const stdio__session_t *session, bruce_process_id_t owner) {
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

bruce_result_t stdio__session_create(bruce_stdio_session_t *out_session) {
    if (out_session == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_session = BRUCE_STDIO_SESSION_INVALID;
    bruce_process_id_t owner = process__current_id();
    stdio__session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) return BRUCE_ERR_NO_MEMORY;

    session->owner = owner;
    session->resource = process_registry__resource_register(stdio__session_cleanup, session);
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
        (void)process_registry__resource_release(session->resource);
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

bruce_result_t stdio__session_close(bruce_stdio_session_t session) {
    bruce_process_id_t owner = process__current_id();
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
    (void)process_registry__resource_release(resource);
    free(entry);
    (void)process_registry__set_child_stdio_session(BRUCE_STDIO_SESSION_INVALID);
    return BRUCE_OK;
}

bruce_result_t stdio__session_route_children(bruce_stdio_session_t session) {
    if (session != BRUCE_STDIO_SESSION_INVALID) {
        bruce_process_id_t owner = process__current_id();
        stdio__ensure_init();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        stdio__session_t *entry = stdio__find_locked(session);
        bool owned = stdio__owned_locked(entry, owner);
        xSemaphoreGive(s_lock);
        if (!owned) return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    return process_registry__set_child_stdio_session(session);
}

bruce_result_t stdio__session_write_input(bruce_stdio_session_t session, const void *data, size_t size) {
    if (data == NULL || size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_id_t owner = process__current_id();
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

bruce_result_t
stdio__session_read_output(bruce_stdio_session_t session, void *buffer, size_t capacity, size_t *out_size) {
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    bruce_process_id_t owner = process__current_id();
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
        if (runtime__delay(0) != BRUCE_OK) { return BRUCE_ERR_CANCELLED; }
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
        uint64_t elapsed = runtime__now() - started;
        if (timeout_ms == 0 || (timeout_ms != UINT32_MAX && elapsed >= timeout_ms)) {
            return BRUCE_ERR_TIMEOUT;
        }
        uint64_t remaining = timeout_ms == UINT32_MAX ? STDIO__READ_POLL_MS : timeout_ms - elapsed;
        uint32_t delay_ms = remaining < STDIO__READ_POLL_MS ? (uint32_t)remaining : STDIO__READ_POLL_MS;
        if (runtime__delay(delay_ms) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
}

/* Discards whatever's currently sitting unread in the session's input queue,
 * the same way a real tty driver flushes pending input when it delivers
 * SIGINT/SIGQUIT to the foreground process (termios' default, NOFLSH unset).
 * Without this, bytes that were already written (e.g. a burst/paste, or
 * stdio__session_write_input() in a test) but not yet read at the moment a
 * blocking stdio__session_read_input() is cancelled stay queued and get
 * silently replayed into whatever reads next -- see shell_app.c's Ctrl+C
 * handling, the first caller that needs this. */
bruce_result_t stdio__session_flush_input(bruce_stdio_session_t session) {
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_NOT_FOUND;
    }
    entry->input_read = 0;
    entry->input_size = 0;
    xSemaphoreGive(s_lock);
    return BRUCE_OK;
}

/* Same as stdio__session_flush_input(), but for the calling process's own
 * routed session (see stdio__read()/stdio__write() above for the same
 * "current session" pattern). A no-op when there's no routed session to
 * flush (e.g. the physical serial console has no such queue to discard). */
bruce_result_t stdio__flush_input(void) {
    bruce_stdio_session_t session = process_registry__current_stdio_session();
    if (session == BRUCE_STDIO_SESSION_INVALID) return BRUCE_OK;
    return stdio__session_flush_input(session);
}

#if !CONFIG_LIBC_PICOLIBC
static int stdio__stream_read(void *cookie, char *buffer, int size) {
    size_t read_size = 0;
    bruce_result_t result = stdio__session_read_input(
        (bruce_stdio_session_t)(uintptr_t)cookie, buffer, (size_t)size, UINT32_MAX, &read_size
    );
    /* funopen cannot represent Bruce errors; cancellation is exposed as EOF. */
    return result == BRUCE_OK ? (int)read_size : 0;
}
#endif

/* Appends a single raw byte to the session's output ring buffer, blocking
 * (with backpressure) until room is available, and updates the session's
 * ONLCR tracking state. Returns false if the session is gone or the wait
 * was cancelled. */
static bool stdio__session_push_output_byte(bruce_stdio_session_t session, char byte) {
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        stdio__session_t *entry = stdio__find_locked(session);
        if (entry == NULL) {
            xSemaphoreGive(s_lock);
            return false;
        }
        if (entry->output_size < STDIO__OUTPUT_CAPACITY) {
            size_t write_at = (entry->output_read + entry->output_size) % STDIO__OUTPUT_CAPACITY;
            entry->output[write_at] = byte;
            entry->output_size++;
            entry->output_last_was_cr = byte == '\r';
            xSemaphoreGive(s_lock);
            return true;
        }
        xSemaphoreGive(s_lock);
        if (runtime__delay(1) != BRUCE_OK) return false;
    }
}

/* Terminal apps (see terminal_app.c) render a fixed grid where '\r' and '\n'
 * are independent ANSI actions, same as a real terminal: '\n' alone only
 * moves down a row, it does not return to column 0. A real serial console
 * gets that translation for free from the OS tty/VFS driver (ONLCR), but a
 * session's output is consumed directly by the terminal grid with no such
 * layer in between, so callers writing a bare '\n' (as every stdio__printf
 * caller does) would print a staircase instead of a newline. Applying the
 * same ONLCR translation here keeps session output behaving like a normal
 * terminal without requiring every caller to spell out "\r\n". */
static bruce_result_t
stdio__session_write_output(bruce_stdio_session_t session, const void *data, size_t size) {
    if (data == NULL || size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    stdio__ensure_init();
    const char *buffer = data;
    for (size_t i = 0; i < size; ++i) {
        char byte = buffer[i];
        if (byte == '\n') {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            stdio__session_t *entry = stdio__find_locked(session);
            if (entry == NULL) {
                xSemaphoreGive(s_lock);
                return BRUCE_ERR_NOT_FOUND;
            }
            bool need_cr = !entry->output_last_was_cr;
            xSemaphoreGive(s_lock);
            if (need_cr && !stdio__session_push_output_byte(session, '\r')) return BRUCE_ERR_CANCELLED;
        }
        if (!stdio__session_push_output_byte(session, byte)) return BRUCE_ERR_CANCELLED;
    }
    return BRUCE_OK;
}

#if !CONFIG_LIBC_PICOLIBC
static int stdio__stream_write(void *cookie, const char *buffer, int size) {
    if (size <= 0) return 0;
    bruce_result_t result =
        stdio__session_write_output((bruce_stdio_session_t)(uintptr_t)cookie, buffer, (size_t)size);
    return result == BRUCE_OK ? size : 0;
}
#endif

void stdio__process_attach(
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

void stdio__process_detach(FILE *input, FILE *output, FILE *error) {
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

bruce_result_t stdio__write_to(bruce_stdio_session_t session, const void *data, size_t size) {
    if (size == 0) return BRUCE_OK;
    if (data == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (session != BRUCE_STDIO_SESSION_INVALID) { return stdio__session_write_output(session, data, size); }

    const char *bytes = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(STDOUT_FILENO, bytes + offset, size - offset);
        if (written <= 0) return BRUCE_ERR_IO;
        offset += (size_t)written;
    }
    return BRUCE_OK;
}

bruce_result_t stdio__write(const void *data, size_t size) {
    return stdio__write_to(process_registry__current_stdio_session(), data, size);
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
    bruce_result_t result = stdio__write(output, (size_t)size);
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

bruce_result_t stdio__read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size) {
    bruce_stdio_session_t session = process_registry__current_stdio_session();
    if (session != BRUCE_STDIO_SESSION_INVALID) {
        return stdio__session_read_input(session, buffer, capacity, timeout_ms, out_size);
    }
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (stdio__init() != BRUCE_OK) return BRUCE_ERR_IO;
    int count = usb_serial_jtag_read_bytes(buffer, capacity, pdMS_TO_TICKS(timeout_ms));
    if (count == 0) return BRUCE_ERR_TIMEOUT;
    if (count < 0) return BRUCE_ERR_IO;
    *out_size = (size_t)count;
    return BRUCE_OK;
#else
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
#endif
}

/* Ctrl+D: not a readline-style binding here (compare
 * shell_console__read_line()'s own SHELL_CONSOLE_CTRL_D, which deletes the
 * character under the cursor mid-line -- that's specific to editing the
 * shell's own command line). This is the plainer convention a real
 * terminal's canonical line discipline gives *any* program reading raw
 * input: on an empty line it's end-of-file; mid-line it flushes what's been
 * typed so far immediately, without waiting for Enter. Lets any caller of
 * stdio__read_line() -- not just the shell -- offer "type input, Ctrl+D when
 * done" the way bash's own `cat` (with no arguments) does. */
#define STDIO__READ_LINE_CTRL_D 0x04

int stdio__read_line(char *buffer, size_t buffer_size, bool mask_input) {
    if (buffer == NULL || buffer_size == 0) return -1;
    bruce_stdio_session_t session = process_registry__current_stdio_session();
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
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            c = ch;
        }
        /* Terminal (see terminal_app.c's terminal__handle_input()) forwards a
         * physical Enter keypress as a bare '\r', never '\n', so both must
         * end the line here or a session-routed read (e.g. the terminal-mode
         * permission Allow/Deny prompt in dialog.c) waits for a '\n' that
         * never arrives and Enter appears to do nothing. */
        if (c == '\n' || c == '\r') break;
        if (c == STDIO__READ_LINE_CTRL_D) {
            if (i == 0) eof = true;
            break;
        }
        if (c == '\b' || c == 0x7f) {
            if (i > 0) {
                i--;
                if (!mask_input) { (void)stdio__write("\b \b", 3); }
            }
            continue;
        }
        buffer[i++] = (char)c;
        if (!mask_input) {
            char byte = (char)c;
            (void)stdio__write(&byte, 1);
        }
    }
    buffer[i] = '\0';
    (void)stdio__write("\n", 1);
    return eof && i == 0 ? -1 : (int)i;
}

bool tty__isatty(void) {
    bruce_stdio_session_t session = process_registry__current_stdio_session();
    if (session == BRUCE_STDIO_SESSION_INVALID) return false;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    bool is_tty = entry != NULL && entry->tty_generation != 0;
    xSemaphoreGive(s_lock);
    return is_tty;
}

bruce_result_t tty__get_size(bruce_tty_size_t *out_size) {
    if (out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_stdio_session_t session = process_registry__current_stdio_session();
    if (session == BRUCE_STDIO_SESSION_INVALID) return BRUCE_ERR_NOT_FOUND;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_NOT_FOUND;
    }
    out_size->columns = entry->tty_columns;
    out_size->rows = entry->tty_rows;
    out_size->generation = entry->tty_generation;
    xSemaphoreGive(s_lock);
    return BRUCE_OK;
}

bruce_result_t tty__set_size(bruce_stdio_session_t session, uint16_t columns, uint16_t rows) {
    if (columns == 0 || rows == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_id_t owner = process__current_id();
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (!stdio__owned_locked(entry, owner)) {
        xSemaphoreGive(s_lock);
        return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    if (entry->tty_columns != columns || entry->tty_rows != rows) {
        entry->tty_columns = columns;
        entry->tty_rows = rows;
        entry->tty_generation++;
    }
    xSemaphoreGive(s_lock);
    return BRUCE_OK;
}

bruce_tty_mode_t tty__get_mode(void) {
    bruce_stdio_session_t session = process_registry__current_stdio_session();
    if (session == BRUCE_STDIO_SESSION_INVALID) return BRUCE_TTY_MODE_COOKED;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    bruce_tty_mode_t mode = entry != NULL ? entry->tty_mode : BRUCE_TTY_MODE_COOKED;
    xSemaphoreGive(s_lock);
    return mode;
}

bruce_result_t tty__set_mode(bruce_tty_mode_t mode) {
    if (mode != BRUCE_TTY_MODE_COOKED && mode != BRUCE_TTY_MODE_RAW) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_stdio_session_t session = process_registry__current_stdio_session();
    if (session == BRUCE_STDIO_SESSION_INVALID) return BRUCE_ERR_NOT_FOUND;
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return BRUCE_ERR_NOT_FOUND;
    }
    entry->tty_mode = mode;
    xSemaphoreGive(s_lock);
    return BRUCE_OK;
}

bruce_result_t tty__get_mode_of(bruce_stdio_session_t session, bruce_tty_mode_t *out_mode) {
    if (out_mode == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_id_t owner = process__current_id();
    stdio__ensure_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stdio__session_t *entry = stdio__find_locked(session);
    if (!stdio__owned_locked(entry, owner)) {
        xSemaphoreGive(s_lock);
        return entry == NULL ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_PERMISSION;
    }
    *out_mode = entry->tty_mode;
    xSemaphoreGive(s_lock);
    return BRUCE_OK;
}
