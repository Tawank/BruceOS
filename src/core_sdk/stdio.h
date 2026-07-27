#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

typedef uint32_t bruce_stdio_session_t;

#define BRUCE_STDIO_SESSION_INVALID ((bruce_stdio_session_t)0)

/* Read a line from stdin, waiting if the console driver is non-blocking.
 * Echoes characters unless mask_input is true.
 * The returned string does NOT include the trailing newline.
 * Returns the number of characters read, or -1 on invalid arguments / EOF. */
int bruce_stdio_read_line(char *buffer, size_t buffer_size, bool mask_input);

/* Reads up to `capacity` raw bytes from stdin without echoing. A zero timeout
 * polls. Returns BRUCE_ERR_TIMEOUT when no input arrives before the deadline. */
bruce_result_t bruce_stdio_read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size);

/* Creates a task-owned, bounded stdin/stdout session. Routing a session makes
 * subsequently launched child tasks read from its input queue and write to
 * its output queue; call route_children(INVALID) after launching. */
bruce_result_t bruce_stdio_session_create(bruce_stdio_session_t *out_session);
bruce_result_t bruce_stdio_session_close(bruce_stdio_session_t session);
bruce_result_t bruce_stdio_session_route_children(bruce_stdio_session_t session);
bruce_result_t bruce_stdio_session_write_input(bruce_stdio_session_t session, const void *data, size_t size);
bruce_result_t bruce_stdio_session_read_output(
    bruce_stdio_session_t session, void *buffer, size_t capacity, size_t *out_size
);
