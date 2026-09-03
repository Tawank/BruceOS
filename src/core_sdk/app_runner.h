#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/environment.h"
#include "core_sdk/result.h"

/**
 * @brief Runs apps and built-in commands.
 */

typedef int (*bruce_app_entry_t)(int argc, char **argv);

typedef enum {
    BRUCE_LAUNCH_FOREGROUND = 0,
    BRUCE_LAUNCH_BACKGROUND = 1,
} bruce_launch_mode_t;

/**
 * @brief Registers a built-in command.
 *
 * Name and description must be non-empty and remain valid for the lifetime
 * of the system. `category` groups commands for docs and command search
 * (`man`, launcher, shell completion) - it has no effect on lookup or
 * execution; pass one of the short, title-case names used in
 * app_runner__register_defaults() (e.g. "System", "Storage", "Network"), or
 * NULL/"" if the command has none (excluded from category listings).
 * Returns BRUCE_ERR_ALREADY_EXISTS for a duplicate name and
 * BRUCE_ERR_RESOURCE_LIMIT if the registry is full.
 *
 * @param name Command name used to invoke it; must be unique.
 * @param description One-line summary shown in command listings/`man`.
 * @param category Grouping label for docs/search, or NULL/"" for none.
 * @param entry Entry point called with conventional C argc/argv.
 * @param stack_bytes Stack size to give the process running this command.
 */
bruce_result_t app_runner__register(
    const char *name, const char *description, const char *category, bruce_app_entry_t entry,
    uint32_t stack_bytes
);

/**
 * @brief One entry of an app_runner__register_all() table.
 *
 * Field-for-field the same as app_runner__register()'s arguments; see its
 * doc comment for what each one means and how long it must stay valid.
 */
typedef struct {
    const char *name;
    const char *description;
    const char *category;
    bruce_app_entry_t entry;
    uint32_t stack_bytes;
} bruce_app_descriptor_t;

/**
 * @brief Registers a whole table of built-in commands in one call.
 *
 * Equivalent to calling app_runner__register() once per entry, in order,
 * except that `apps` is referenced in place rather than copied - it must be
 * `static const` (or otherwise outlive the system), exactly like the
 * arguments to app_runner__register() already must. Every entry is
 * validated up front, so a malformed or duplicate entry is rejected before
 * any of the table is installed, and this returns whichever
 * app_runner__register() error that entry would have returned.
 *
 * @param apps Table of command descriptors; must remain valid for the
 *             lifetime of the system.
 * @param count Number of entries in `apps`.
 */
bruce_result_t app_runner__register_all(const bruce_app_descriptor_t *apps, size_t count);

/**
 * @brief Returns the number of registered built-in commands.
 *
 * Entries are returned in registration order and remain owned by AppRunner.
 * Returns NULL when `index` is out of range.
 */
size_t app_runner__command_count(void);

/**
 * @brief Name of the built-in command at `index`.
 *
 * @param index Zero-based index into the command registry.
 */
const char *app_runner__command_name(size_t index);

/**
 * @brief Description of the built-in command at `index`.
 *
 * @param index Zero-based index into the command registry.
 */
const char *app_runner__command_description(size_t index);

/**
 * @brief Category of the built-in command at `index`.
 *
 * @param index Zero-based index into the command registry.
 */
const char *app_runner__command_category(size_t index);

/**
 * @brief Starts a named built-in or loader-registered application.
 *
 * See core_sdk/ext_mem_loader.h. On success this returns a positive
 * bruce_process_id_t. On failure it returns a negative BRUCE_ERR_* value
 * (including BRUCE_ERR_NOT_FOUND and BRUCE_ERR_BUSY). `arg` is shell-style
 * text. Registered built-ins use conventional C arguments: argv[0] is
 * `app_name`, argv[argc] is NULL, and NULL or an empty `arg` therefore
 * creates argc == 1. Loader-resolved applications define their own argv[0]
 * convention.
 *
 * @param app_name Name of the built-in or loader-registered app to start.
 * @param arg Shell-style argument text, or NULL/"" for none.
 * @param mode Whether to launch in the foreground or background.
 * @permission execute (external callers only; built-ins always pass)
 */
int app_runner__run(const char *app_name, const char *arg, bruce_launch_mode_t mode);

/**
 * @brief Starts an app with temporary environment variables.
 *
 * The overlay is deep-copied before this function returns.
 *
 * @param app_name Name of the built-in or loader-registered app to start.
 * @param arg Shell-style argument text, or NULL/"" for none.
 * @param mode Whether to launch in the foreground or background.
 * @param environment Extra environment variables to give the child.
 * @param environment_count Number of entries in environment.
 * @permission execute (external callers only; built-ins always pass)
 */
int app_runner__run_with_environment(
    const char *app_name, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);

/**
 * @brief Runs a command line, including environment variable assignments.
 *
 * An explicit BG=0 or BG=1 selects foreground/background; otherwise
 * default_mode is used. GUI=1 requests a GUI process. OVERLAY=1 preserves
 * the visible framebuffer for an overlay-only GUI process. These
 * assignments are still included in the child's environment.
 *
 * @param command_line Full command line, e.g. "app_name arg1 arg2".
 * @param default_mode Launch mode used when no explicit BG= assignment is present.
 */
bruce_result_t app_runner__run_command(const char *command_line, bruce_launch_mode_t default_mode);

/**
 * @brief Splits a command line into arguments.
 *
 * Ensures quoting/escaping rules are identical everywhere: splits on runs of
 * spaces/tabs; supports single quotes (fully literal), double quotes
 * (backslash escapes only `\"` and `\\`), and a bare backslash outside
 * quotes to escape the next character. NULL or an empty `arg` produces
 * argc == 0 with *out_argv left NULL. Returns BRUCE_ERR_INVALID_ARGUMENT for
 * an unterminated quote or a trailing unescaped backslash, and
 * BRUCE_ERR_NO_MEMORY on allocation failure. The caller must free a
 * successful result with app_runner__free_args().
 *
 * @param arg Shell-style argument text to tokenize.
 * @param out_argv Receives a newly allocated argv array.
 * @param out_argc Receives the number of tokens in *out_argv.
 */
bruce_result_t app_runner__parse_args(const char *arg, char ***out_argv, int *out_argc);

/**
 * @brief Frees an argv produced by app_runner__parse_args().
 *
 * Safe to call with argv == NULL (e.g. when argc == 0).
 *
 * @param argv Array previously returned via app_runner__parse_args()'s out_argv.
 * @param argc Token count previously returned via app_runner__parse_args()'s out_argc.
 */
void app_runner__free_args(char **argv, int argc);

/**
 * @brief Returns the configured icon for a file path.
 *
 * "file" when no icon is configured for its extension. The returned string
 * is owned by Core.
 *
 * @param path File path whose extension selects the icon.
 */
const char *app_runner__icon_for_path(const char *path);

/**
 * @brief Checks whether an environment requests a graphical interface.
 *
 * Not yet applied to any process. Returns true iff the last matching
 * entry's value is "1". Shared by app_runner__run()'s built-in path and by
 * loader modules, which must determine this for the process context they
 * are about to spawn before it exists (see migration_plan.md, "Dialog and
 * process interaction").
 *
 * @param environment Environment overlay to scan.
 * @param environment_count Number of entries in environment.
 */
bool app_runner__environment_requests_gui(
    const bruce_environment_variable_t *environment, size_t environment_count
);
