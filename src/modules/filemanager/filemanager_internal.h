#pragma once

/* Shared between the pieces filemanager_app.c's single 1000-line file was
 * split into -- filemanager_actions.c (single-entry file/folder actions),
 * filemanager_clipboard.c (copy/paste), and filemanager_network.c (the
 * "/Network" folder feature). filemanager_app.c keeps these implementations
 * (they're small and every other file needs at least one of them) and owns
 * the main() dispatch loop that wires the rest together. Not part of the
 * public core_sdk/ API: other modules must not include this header, only
 * filemanager_app.h.
 */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

/* True if the calling process was foregrounded again after being sent to
 * the background (e.g. by a hotkey) and is blocking on that; waits for the
 * transition back to BRUCE_PROCESS_FOREGROUND. False if it was never
 * backgrounded, so the caller's own input__read()/dialog__choice() error
 * handling should run instead. Used by both filemanager_app.c's main loop
 * and filemanager__view_file()'s own input loop, each time a read comes
 * back BRUCE_ERR_NOT_FOREGROUND / BRUCE_ERR_CANCELLED. */
bool filemanager__resume_after_handoff(void);

/* Basename (the part after the last '/', or the whole string if there is
 * none) of a storage path. Never fails -- always returns a pointer into
 * `path` itself. */
const char *filemanager__basename(const char *path);

/* Writes `path`'s parent directory into `out` ("/" for a top-level entry or
 * `path` itself when it has no '/'). */
void filemanager__parent_path(const char *path, char *out, size_t out_size);

/* Shell-escapes `path` (spaces/tabs/backslash/quotes get a backslash) into
 * `out`, for building an argument string handed to app_runner__run() et al.
 * Returns false if `out` is too small. */
bool filemanager__escape_arg(const char *path, char *out, size_t out_size);

/* Runs `app` with `path` as its argument (as "-r <path>" when `read_only`),
 * foreground, with GUI=1 set when `gui` is true, and waits for it to exit. */
bruce_result_t
filemanager__run_named_app(const char *app, const char *path, bool gui, bool read_only);

/* Shows `result` as an error dialog titled "Apps", with `action` folded into
 * the message (see core_sdk/ext_mem_loader.h's format_error_message()). */
void filemanager__show_error(const char *action, bruce_result_t result);
