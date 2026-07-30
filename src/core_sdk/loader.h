#pragma once

/*
 * Loader registry (see migration_plan.md, "Loader modules").
 *
 * A loader module turns a file of one specific extension into a running
 * Core process by registering itself here.  Core ships built-in ELF
 * (".elf", priority 10) and JavaScript (".js", priority 20) loader modules,
 * but neither gets any Core access that a third-party loader module (a
 * ".py" loader, for example) registering the same way would not also get.
 */

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* Matches app_runner__run_path()'s signature. */
typedef int (*bruce_loader_run_fn)(const char *path, const char *arg, bool in_background);

/* Entry point for a process created by app_runner__spawn_loader_process();
 * `context` is the loader's own opaque pointer (e.g. a struct holding the
 * decoded image or script source and its own argc/argv). */
typedef void (*bruce_loader_process_entry_fn)(void *context);

/* Registers a loader for `extension` (must start with '.', e.g. ".elf").
 * `priority` breaks ties when app_runner__run()'s named resolution finds
 * more than one candidate file for the same app name; lower values are
 * tried first.  Registration happens once at boot, before the first
 * named-run or path-run call.  Returns BRUCE_ERR_ALREADY_EXISTS for a
 * duplicate extension and BRUCE_ERR_RESOURCE_LIMIT if the registry is
 * full. */
bruce_result_t app_runner__register_loader(const char *extension, int priority, bruce_loader_run_fn run_fn);

/* Starts an arbitrary absolute path via whichever loader is registered for
 * its extension.  Used directly by `execute`-permission callers (file
 * managers, etc.) and internally by app_runner__run()'s named resolution.
 * `path` must be normalized with no "." or ".." components.  Returns a
 * positive bruce_process_id_t on success; BRUCE_ERR_INVALID_PATH,
 * BRUCE_ERR_NOT_FOUND (no loader registered for the extension), or the
 * loader's own negative BRUCE_ERR_* on failure. External callers require the
 * `execute` permission. */
int app_runner__run_path(const char *path, const char *arg, bool in_background);

/* The one extra public primitive a loader module needs beyond
 * app_runner__register(): creates and starts a real Core process that calls
 * entry(context) on its own stack, wired into the same foreground stack,
 * resource registry, and permission checks (`permission_key`, e.g.
 * "game.elf") as any other process - without any private Core header.
 * `stack_size` of 0 selects a Core default.  Returns a positive
 * bruce_process_id_t on success or a negative BRUCE_ERR_*. */
int app_runner__spawn_loader_process(
    const char *permission_key, bool gui_requested, bool in_background, uint32_t stack_size,
    bruce_loader_process_entry_fn entry, void *context
);
