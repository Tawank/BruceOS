#pragma once

/**
 * @brief Terminal size and mode settings.
 *
 * BruceOS's stdio__session_t (core/stdio/stdio.c) already behaves like a
 * pty: it's a process-owned, bounded input/output byte channel inherited
 * transitively down a process tree (see stdio__session_route_children).
 * There is no separate line-discipline layer -- nothing at the OS level
 * echoes or edits input, so "raw mode" is already the default behavior for
 * any reader that isn't itself an editor (see stdio__read). What's missing
 * is terminal *geometry* and a place for programs to agree on it, which is
 * what this header adds: a size (columns/rows) with a generation counter
 * for polling-based resize detection, and a cooperative mode flag for
 * bookkeeping/introspection (`stty`).
 *
 * Not permission-gated (ownership checks are used instead).
 */

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

typedef enum {
    BRUCE_TTY_MODE_COOKED = 0,
    BRUCE_TTY_MODE_RAW = 1,
} bruce_tty_mode_t;

typedef struct {
    uint16_t columns;
    uint16_t rows;
    /* Increments on every tty__set_size call that changes the size. Poll
     * this from a redraw loop to detect a resize without any signal --
     * BruceOS has no SIGWINCH equivalent by design (see process.h's
     * single-slot cooperative signal model), so this is the resize path. */
    uint32_t generation;
} bruce_tty_size_t;

/**
 * @brief True when the calling process's routed stdio session has a known terminal size.
 *
 * i.e. some owner (a terminal app, the ssh client, ...) has called
 * tty__set_size on it at least once. False when no session is routed (the
 * physical console) or the session's size was never set (a plain pipe).
 */
bool tty__isatty(void);

/**
 * @brief Reads the calling process's routed session's terminal geometry.
 *
 * Returns BRUCE_ERR_NOT_FOUND if no session is routed.
 *
 * @param out_size Receives the terminal geometry.
 */
bruce_result_t tty__get_size(bruce_tty_size_t *out_size);

/**
 * @brief Sets the terminal geometry of `session`.
 *
 * Only the session's owner (the process that created it -- typically the
 * terminal app or the ssh client) may call this: it represents a physical
 * resize, not a request from a tenant process running inside the session.
 * Returns BRUCE_ERR_PERMISSION if the caller isn't the owner,
 * BRUCE_ERR_NOT_FOUND if the session doesn't exist.
 *
 * @param session Session to resize; caller must be its owner.
 * @param columns New terminal width in columns.
 * @param rows New terminal height in rows.
 */
bruce_result_t tty__set_size(bruce_stdio_session_t session, uint16_t columns, uint16_t rows);

/**
 * @brief Reads the terminal mode.
 *
 * BruceOS never echoes or line-edits at the session level, so switching
 * modes changes no OS behavior by itself. This exists so a process can
 * declare its own intent (surfaced by `stty`, and as the landing spot for
 * any future tcsetattr()-shaped compatibility shim) and so a nested reader
 * can tell whether the terminal already considers itself raw. Operates on
 * the calling process's own routed session; no ownership check, since it's
 * just self-description.
 */
bruce_tty_mode_t tty__get_mode(void);

/**
 * @brief Sets the cooperative mode of the calling process's own routed session.
 *
 * Cooperative bookkeeping only -- see tty__get_mode().
 *
 * @param mode New cooperative mode.
 */
bruce_result_t tty__set_mode(bruce_tty_mode_t mode);
