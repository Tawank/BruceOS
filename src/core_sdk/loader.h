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
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"

/* Matches app_runner__run_path_with_environment()'s internal dispatch. */
typedef int (*bruce_loader_run_fn)(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);

/* Entry point for a process created by app_runner__spawn_loader_process();
 * `context` is the loader's own opaque pointer (e.g. a struct holding the
 * decoded image or script source and its own argc/argv). */
typedef void (*bruce_loader_process_entry_fn)(void *context);
typedef void (*bruce_loader_process_cleanup_fn)(void *context);

typedef struct {
    const uint8_t *data;
    size_t size;
    bruce_memory_object_t memory;
} bruce_loader_t;

typedef struct {
    const uint8_t *instruction;
    const uint8_t *data;
    size_t size;
    bruce_memory_object_t memory;
} bruce_loader_xip_image_t;

/* Streams a file into process-owned external memory, verifies it, and returns
 * a read-only mapping. data[size] is a trailing NUL byte outside the image
 * size, allowing parsers that require sentinel-terminated input to use the
 * mapping directly. The caller must release successful mappings promptly. */
bruce_result_t loader__stage_path(const char *path, bruce_loader_t *out_image);
/* Transfers a staged image to the current process after a loader spawns it. */
bruce_result_t loader__adopt_image(bruce_loader_t *image);
bruce_result_t loader__release_image(bruce_loader_t *image);

/* Allocates executable MMU-page-exclusive space in swap. The mapping
 * remains valid until release; writes are bounds checked and cache coherent. */
bruce_result_t loader__allocate_xip(size_t size, bruce_loader_xip_image_t *out_image);
bruce_result_t
loader__write_xip(const bruce_loader_xip_image_t *image, size_t offset, const void *data, size_t size);
bruce_result_t loader__adopt_xip(bruce_loader_xip_image_t *image);
bruce_result_t loader__release_xip(bruce_loader_xip_image_t *image);

/* Loader implementations may retain a concise diagnostic for the launcher to
 * show when their launch attempt fails. The text is cleared by passing NULL. */
void loader__set_error_message(const char *message);
const char *loader__last_error_message(void);

/* Formats a user-facing launch failure. ABI mismatches explain that the app
 * needs a newer Bruce version; other results use the SDK error description. */
void loader__format_error_message(const char *action, int result, char *out_message, size_t out_size);

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
int app_runner__run_path(const char *path, const char *arg, bruce_launch_mode_t mode);
int app_runner__run_path_with_environment(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);

/* The one extra public primitive a loader module needs beyond
 * app_runner__register(): creates and starts a real Core process that calls
 * entry(context) on its own stack, wired into the same foreground stack,
 * resource registry, and permission checks (`permission_key`, e.g.
 * "game.elf") as any other process - without any private Core header.
 * `stack_size` of 0 selects a Core default.  Returns a positive
 * bruce_process_id_t on success or a negative BRUCE_ERR_*. */
int app_runner__spawn_loader_process(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context
);

/* Variant that transfers ownership of context to Core. cleanup runs exactly
 * once after normal return, force-kill, or cancellation before entry starts. */
int app_runner__spawn_loader_process_owned(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context, bruce_loader_process_cleanup_fn cleanup
);
