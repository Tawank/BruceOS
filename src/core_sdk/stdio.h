#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Read a line from stdin, waiting if the console driver is non-blocking.
 * Echoes characters unless mask_input is true.
 * The returned string does NOT include the trailing newline.
 * Returns the number of characters read, or -1 on invalid arguments / EOF. */
int bruce_stdio_read_line(char *buffer, size_t buffer_size, bool mask_input);
