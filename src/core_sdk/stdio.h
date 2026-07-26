#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* Read a line from stdin, waiting if the console driver is non-blocking.
 * Echoes characters unless mask_input is true.
 * The returned string does NOT include the trailing newline.
 * Returns the number of characters read, or -1 on invalid arguments / EOF. */
int bruce_stdio_read_line(char *buffer, size_t buffer_size, bool mask_input);

/* Reads up to `capacity` raw bytes from stdin without echoing. A zero timeout
 * polls. Returns BRUCE_ERR_TIMEOUT when no input arrives before the deadline. */
bruce_result_t bruce_stdio_read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size);
