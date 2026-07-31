#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

typedef uint32_t bruce_stdio_session_t;

#define BRUCE_STDIO_SESSION_INVALID ((bruce_stdio_session_t)0)

/* Read a line from stdin, waiting if the console driver is non-blocking.
 * Echoes characters unless mask_input is true.
 * The returned string does NOT include the trailing newline.
 * Returns the number of characters read, BRUCE_ERR_CANCELLED if the calling
 * process receives cooperative INT/TERM, or -1 on invalid arguments / EOF. */
int stdio__read_line(char *buffer, size_t buffer_size, bool mask_input);

/* Reads up to `capacity` raw bytes from stdin without echoing. A zero timeout
 * polls and UINT32_MAX waits indefinitely. Finite waits use the original
 * absolute deadline. Returns BRUCE_ERR_TIMEOUT when no input arrives before
 * that deadline, or BRUCE_ERR_CANCELLED if the calling process receives
 * cooperative INT/TERM. Cancellation takes precedence over queued input. */
bruce_result_t stdio__read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size);

/* Writes app-visible output to the calling process's routed session, or to the
 * physical serial console when no session is routed. Core diagnostics should
 * continue to use the normal logging/stdio functions. */
bruce_result_t stdio__write(const void *data, size_t size);

/* printf-style form of bruce_stdio_write(). Returns the number of formatted
 * bytes written, or a negative BRUCE_ERR_* value. */
int stdio__printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int stdio__vprintf(const char *format, va_list args) __attribute__((format(printf, 1, 0)));

/* Creates a process-owned, bounded input/output session. Routing a session makes
 * subsequently launched child processes use it through bruce_stdio_read(),
 * bruce_stdio_write(), and stdio__printf(); call route_children(INVALID)
 * after launching. */
bruce_result_t stdio__session_create(bruce_stdio_session_t *out_session);
bruce_result_t stdio__session_close(bruce_stdio_session_t session);
bruce_result_t stdio__session_route_children(bruce_stdio_session_t session);
bruce_result_t stdio__session_write_input(bruce_stdio_session_t session, const void *data, size_t size);
bruce_result_t
stdio__session_read_output(bruce_stdio_session_t session, void *buffer, size_t capacity, size_t *out_size);
