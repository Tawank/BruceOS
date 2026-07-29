#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

typedef int (*bruce_app_entry_t)(int argc, char **argv);

/* Registers a built-in command.  Returns BRUCE_ERR_ALREADY_EXISTS for a
 * duplicate name and BRUCE_ERR_RESOURCE_LIMIT if the registry is full. */
bruce_result_t app_runner__register(const char *name, bruce_app_entry_t entry);

/* Read-only access to registered built-in command names. Names are returned in
 * registration order and remain owned by AppRunner. Returns NULL when `index`
 * is out of range. */
size_t app_runner__command_count(void);
const char *app_runner__command_name(size_t index);

/* Starts a named built-in or loader-registered application (see
 * core_sdk/loader.h).  On success this returns a positive bruce_task_id_t.
 * On failure it returns a negative BRUCE_ERR_* value (including
 * BRUCE_ERR_NOT_FOUND and BRUCE_ERR_BUSY). External callers require the
 * `execute` permission. `arg` is shell-style text. Registered built-ins use
 * conventional C arguments: argv[0] is `app_name`, argv[argc] is NULL, and
 * NULL or an empty `arg` therefore creates argc == 1. Loader-resolved
 * applications define their own argv[0] convention. */
int app_runner__run(const char *app_name, const char *arg, bool in_background);

/* Shell-style tokenizer shared by app_runner__run()'s own named resolution
 * and by every loader module's run_fn, so quoting/escaping rules are
 * identical everywhere: splits on runs of spaces/tabs; supports single
 * quotes (fully literal), double quotes (backslash escapes only `\"` and
 * `\\`), and a bare backslash outside quotes to escape the next character.
 * NULL or an empty `arg` produces argc == 0 with *out_argv left NULL.
 * Returns BRUCE_ERR_INVALID_ARGUMENT for an unterminated quote or a
 * trailing unescaped backslash, and BRUCE_ERR_NO_MEMORY on allocation
 * failure.  The caller must free a successful result with
 * app_runner__free_args(). */
bruce_result_t app_runner__parse_args(const char *arg, char ***out_argv, int *out_argc);

/* Frees an argv produced by app_runner__parse_args().  Safe to call with
 * argv == NULL (e.g. when argc == 0). */
void app_runner__free_args(char **argv, int argc);

/* Returns true if any element of argv is exactly "--gui".  Shared by
 * app_runner__run()'s built-in path and by loader modules, which must parse
 * their own raw `arg` string (see app_runner__parse_args()) to determine
 * this for the task context they spawn (see migration_plan.md, "Dialog
 * and task interaction"). */
bool app_runner__args_have_gui(int argc, char *const *argv);

/* Returns true if any argv element is exactly "--bg". GUI applications use
 * this to suppress their normal startup foreground claim. */
bool app_runner__args_have_background(int argc, char *const *argv);
