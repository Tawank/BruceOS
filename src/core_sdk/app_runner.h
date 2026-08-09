#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/environment.h"
#include "core_sdk/result.h"

typedef int (*bruce_app_entry_t)(int argc, char **argv);

typedef enum {
    BRUCE_LAUNCH_FOREGROUND = 0,
    BRUCE_LAUNCH_BACKGROUND = 1,
} bruce_launch_mode_t;

/* Registers a built-in command.  Returns BRUCE_ERR_ALREADY_EXISTS for a
 * duplicate name and BRUCE_ERR_RESOURCE_LIMIT if the registry is full. */
bruce_result_t app_runner__register(const char *name, bruce_app_entry_t entry, uint32_t stack_bytes);

/* Read-only access to registered built-in command names. Names are returned in
 * registration order and remain owned by AppRunner. Returns NULL when `index`
 * is out of range. */
size_t app_runner__command_count(void);
const char *app_runner__command_name(size_t index);

/* Starts a named built-in or loader-registered application (see
 * core_sdk/loader.h).  On success this returns a positive bruce_process_id_t.
 * On failure it returns a negative BRUCE_ERR_* value (including
 * BRUCE_ERR_NOT_FOUND and BRUCE_ERR_BUSY). External callers require the
 * `execute` permission. `arg` is shell-style text. Registered built-ins use
 * conventional C arguments: argv[0] is `app_name`, argv[argc] is NULL, and
 * NULL or an empty `arg` therefore creates argc == 1. Loader-resolved
 * applications define their own argv[0] convention. */
int app_runner__run(const char *app_name, const char *arg, bruce_launch_mode_t mode);

/* Launches with temporary child environment assignments. The overlay is
 * deep-copied before this function returns. */
int app_runner__run_with_environment(
    const char *app_name, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);

/* Parses a complete command line, including leading NAME=value assignments.
 * An explicit BG=0 or BG=1 selects foreground/background; otherwise
 * default_mode is used. GUI=1 requests a GUI process. OVERLAY=1 preserves the
 * visible framebuffer for an overlay-only GUI process. These assignments are
 * still included in the child's environment. */
bruce_result_t app_runner__run_command(const char *command_line, bruce_launch_mode_t default_mode);

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

/* Translates a bruce_result_t (or the BRUCE_OK/BRUCE_ERR_* range of any int
 * returned by the app_runner__run*() family) into a short human-readable
 * description, e.g. BRUCE_ERR_NOT_FOUND -> "Not found". Positive values
 * (process ids) and unrecognized codes return "Unknown error". Always
 * returns a non-NULL, statically-allocated string. */
const char *app_runner__result_to_string(int result);

/* Scans an environment overlay array (not yet applied to any process) for
 * name "GUI"; returns true iff the last matching entry's value is "1".
 * Shared by app_runner__run()'s built-in path and by loader modules, which
 * must determine this for the process context they are about to spawn
 * before it exists (see migration_plan.md, "Dialog and process
 * interaction"). */
bool app_runner__environment_requests_gui(
    const bruce_environment_variable_t *environment, size_t environment_count
);
