#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Standard input and output.
 */

typedef uint32_t bruce_stdio_session_t;

#define BRUCE_STDIO_SESSION_INVALID ((bruce_stdio_session_t)0)

/**
 * @brief Read a line from stdin, waiting if the console driver is non-blocking.
 *
 * Echoes characters unless mask_input is true. The returned string does
 * NOT include the trailing newline. Returns the number of characters read,
 * BRUCE_ERR_CANCELLED if the calling process receives cooperative
 * INT/TERM, or -1 on invalid arguments / EOF.
 *
 * @param buffer Buffer to receive the line (without a trailing newline).
 * @param buffer_size Size of buffer in bytes.
 * @param mask_input If true, suppresses echoing typed characters.
 */
int stdio__read_line(char *buffer, size_t buffer_size, bool mask_input);

/**
 * @brief Reads up to `capacity` raw bytes from stdin without echoing.
 *
 * A zero timeout polls and UINT32_MAX waits indefinitely. Finite waits use
 * the original absolute deadline. Returns BRUCE_ERR_TIMEOUT when no input
 * arrives before that deadline, or BRUCE_ERR_CANCELLED if the calling
 * process receives cooperative INT/TERM. Cancellation takes precedence
 * over queued input.
 *
 * @param buffer Buffer to receive read bytes.
 * @param capacity Size of buffer in bytes.
 * @param timeout_ms 0 to poll, or UINT32_MAX to wait indefinitely.
 * @param out_size Receives the number of bytes read.
 */
bruce_result_t stdio__read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size);

/**
 * @brief Writes app-visible output to the calling process's routed session.
 *
 * Or to the physical serial console when no session is routed. Core
 * diagnostics should continue to use the normal logging/stdio functions.
 *
 * @param data Bytes to write.
 * @param size Number of bytes in data.
 */
bruce_result_t stdio__write(const void *data, size_t size);

/**
 * @brief Same as stdio__write(), but targets an explicit session.
 *
 * Instead of the calling process's own routed session -- for the rare case
 * (e.g. a background service rendering console fallback output on another
 * process's behalf, see modules/notification_service) where the writer and
 * the intended reader are not the same process. BRUCE_STDIO_SESSION_INVALID
 * writes to the physical serial console, same as stdio__write() with no
 * session routed.
 *
 * @param session Session to write to, or BRUCE_STDIO_SESSION_INVALID for the physical console.
 * @param data Bytes to write.
 * @param size Number of bytes in data.
 */
bruce_result_t stdio__write_to(bruce_stdio_session_t session, const void *data, size_t size);

/**
 * @brief printf-style form of bruce_stdio_write().
 *
 * Returns the number of formatted bytes written, or a negative BRUCE_ERR_*
 * value.
 *
 * @param format printf-style format string.
 * @param ... Arguments matching format's conversion specifiers.
 */
int stdio__printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief va_list form of stdio__printf().
 *
 * @param format printf-style format string.
 * @param args Arguments matching format, as a va_list.
 */
int stdio__vprintf(const char *format, va_list args) __attribute__((format(printf, 1, 0)));

/**
 * @brief Creates a process-owned, bounded input/output session.
 *
 * @param out_session Receives the new session handle.
 */
bruce_result_t stdio__session_create(bruce_stdio_session_t *out_session);

/**
 * @brief Closes a session created by stdio__session_create().
 *
 * @param session Session to close.
 */
bruce_result_t stdio__session_close(bruce_stdio_session_t session);

/**
 * @brief Routes subsequently launched child processes' stdio through `session`.
 *
 * Routing a session makes subsequently launched child processes use it
 * through bruce_stdio_read(), bruce_stdio_write(), and stdio__printf();
 * call route_children(INVALID) after launching to restore the default of
 * routing children into the calling process's own session -- it does not
 * disconnect them, so plain commands launched afterward keep going to the
 * same terminal as before.
 *
 * @param session Session to route children through, or BRUCE_STDIO_SESSION_INVALID to restore the default.
 */
bruce_result_t stdio__session_route_children(bruce_stdio_session_t session);

/**
 * @brief Writes input bytes into a session, as if typed by its consumer.
 *
 * @param session Session to write into.
 * @param data Bytes to write.
 * @param size Number of bytes in data.
 */
bruce_result_t stdio__session_write_input(bruce_stdio_session_t session, const void *data, size_t size);

/**
 * @brief Reads output bytes a session's owning process has written.
 *
 * @param session Session to read from.
 * @param buffer Buffer to receive read bytes.
 * @param capacity Size of buffer in bytes.
 * @param out_size Receives the number of bytes read.
 */
bruce_result_t
stdio__session_read_output(bruce_stdio_session_t session, void *buffer, size_t capacity, size_t *out_size);
