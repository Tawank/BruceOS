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

/* Waits for a foreground child to exit or be sent to the background (minimized/switched away). */
bruce_result_t filemanager__wait_child(int process);

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
