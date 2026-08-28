#pragma once

/**
 * @brief Loader registry (see migration_plan.md, "Loader modules").
 *
 * A loader module turns a file of one specific extension into a running
 * Core process by registering itself here. Core ships built-in ELF (".elf")
 * and JavaScript (".js") loader modules, but neither gets any Core access
 * that a third-party loader module (a ".py" loader, for example)
 * registering the same way would not also get.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"

/* Entry point for a process created by app_runner__spawn_loader_process();
 * `context` is the loader's own opaque pointer (e.g. a struct holding the
 * decoded image or script source and its own argc/argv). */
typedef int (*bruce_loader_process_entry_fn)(void *context);
typedef void (*bruce_loader_process_cleanup_fn)(void *context);
/* Optional non-blocking hook invoked when INT/TERM is delivered. It must only
 * request interruption; owned cleanup still runs through the normal process
 * teardown path. */
typedef void (*bruce_loader_process_stop_fn)(void *context, bruce_process_signal_t signal);

typedef struct {
    const uint8_t *data;
    size_t size;
    bruce_memory_object_t memory;
} bruce_ext_mem_loader_image_t;

typedef struct {
    const uint8_t *instruction;
    const uint8_t *data;
    size_t size;
    bruce_memory_object_t memory;
} bruce_ext_mem_loader_xip_image_t;

/**
 * @brief Streams a file into process-owned external memory, verifies it, and returns a read-only mapping.
 *
 * data[size] is a trailing NUL byte outside the image size, allowing
 * parsers that require sentinel-terminated input to use the mapping
 * directly. The caller must release successful mappings promptly.
 *
 * @param path Path of the file to stream in.
 * @param out_image Receives the read-only mapping.
 */
bruce_result_t ext_mem_loader__stage_path(const char *path, bruce_ext_mem_loader_image_t *out_image);

/**
 * @brief Transfers a staged image to the current process after a loader spawns it.
 *
 * @param image Image staged by ext_mem_loader__stage_path().
 */
bruce_result_t ext_mem_loader__adopt_image(bruce_ext_mem_loader_image_t *image);

/**
 * @brief Releases an image staged/adopted via ext_mem_loader__stage_path()/adopt_image().
 *
 * @param image Image to release.
 */
bruce_result_t ext_mem_loader__release_image(bruce_ext_mem_loader_image_t *image);

/**
 * @brief Allocates executable MMU-page-exclusive space in swap.
 *
 * The mapping remains valid until release; writes are bounds checked and
 * cache coherent.
 *
 * @param size Number of bytes to allocate.
 * @param out_image Receives the allocated XIP image.
 */
bruce_result_t ext_mem_loader__allocate_xip(size_t size, bruce_ext_mem_loader_xip_image_t *out_image);

/**
 * @brief Writes into a bounds-checked region of an allocated XIP image.
 *
 * @param image XIP image allocated via ext_mem_loader__allocate_xip().
 * @param offset Byte offset within the image to write at.
 * @param data Bytes to write.
 * @param size Number of bytes to write.
 */
bruce_result_t
ext_mem_loader__write_xip(
    const bruce_ext_mem_loader_xip_image_t *image, size_t offset, const void *data, size_t size
);

/**
 * @brief Transfers a staged XIP image to the current process after a loader spawns it.
 *
 * @param image XIP image to adopt.
 */
bruce_result_t ext_mem_loader__adopt_xip(bruce_ext_mem_loader_xip_image_t *image);

/**
 * @brief Releases an XIP image allocated/adopted via allocate_xip()/adopt_xip().
 *
 * @param image XIP image to release.
 */
bruce_result_t ext_mem_loader__release_xip(bruce_ext_mem_loader_xip_image_t *image);

/**
 * @brief Sets the loader implementation's own concise diagnostic for the launcher to show.
 *
 * Shown when their launch attempt fails. The text is cleared by passing
 * NULL.
 *
 * @param message Diagnostic text to retain, or NULL to clear it.
 */
void ext_mem_loader__set_error_message(const char *message);

/** @brief Reads back the diagnostic set via ext_mem_loader__set_error_message(). */
const char *ext_mem_loader__last_error_message(void);

/**
 * @brief Formats a user-facing launch failure.
 *
 * ABI mismatches explain that the app needs a newer Bruce version; other
 * results use the SDK error description.
 *
 * @param action Short description of the action that failed, e.g. "launch".
 * @param result BRUCE_ERR_* result code to format.
 * @param out_message Buffer to receive the formatted message.
 * @param out_size Size of out_message in bytes.
 */
void ext_mem_loader__format_error_message(const char *action, int result, char *out_message, size_t out_size);

/**
 * @brief Registers `program` to handle files ending in `extension`.
 *
 * (which must start with '.', e.g. ".elf"). `program` is resolved through
 * the normal AppRunner command registry and receives the matched path as
 * its first argument. Registration order determines named resolution.
 *
 * @param extension File extension to register for, including the leading '.'.
 * @param program Name of the registered AppRunner command that handles it.
 */
bruce_result_t app_runner__register_loader(const char *extension, const char *program);

/**
 * @brief Starts an arbitrary absolute path via whichever loader is registered for its extension.
 *
 * Used directly by `execute`-permission callers (file managers, etc.) and
 * internally by app_runner__run()'s named resolution. `path` must be
 * normalized with no "." or ".." components. Returns a positive
 * bruce_process_id_t on success; BRUCE_ERR_INVALID_PATH,
 * BRUCE_ERR_NOT_FOUND (no loader registered for the extension), or the
 * loader's own negative BRUCE_ERR_* on failure.
 *
 * @param path Normalized absolute path of the file to run.
 * @param arg Shell-style argument text, or NULL/"" for none.
 * @param mode Whether to launch in the foreground or background.
 * @permission execute (external callers only; built-ins always pass)
 */
int app_runner__run_path(const char *path, const char *arg, bruce_launch_mode_t mode);

/**
 * @brief Like app_runner__run_path(), with temporary child environment assignments.
 *
 * @param path Normalized absolute path of the file to run.
 * @param arg Shell-style argument text, or NULL/"" for none.
 * @param mode Whether to launch in the foreground or background.
 * @param environment Extra environment variables to give the child.
 * @param environment_count Number of entries in environment.
 * @permission execute (external callers only; built-ins always pass)
 */
int app_runner__run_path_with_environment(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
);

/**
 * @brief Creates and starts a real Core process that calls entry(context) on its own stack.
 *
 * The one extra public primitive a loader module needs beyond
 * app_runner__register(): wired into the same foreground stack, resource
 * registry, and permission checks (`permission_key`, e.g. "game.elf") as
 * any other process - without any private Core header. `stack_size` of 0
 * selects a Core default. Returns a positive bruce_process_id_t on success
 * or a negative BRUCE_ERR_*.
 *
 * @param permission_key Permission-check key for the new process, e.g. "game.elf".
 * @param gui_requested Whether the new process should be a GUI process.
 * @param mode Whether to launch in the foreground or background.
 * @param stack_size Stack size in bytes for the new process, or 0 for the Core default.
 * @param environment Extra environment variables to give the process.
 * @param environment_count Number of entries in environment.
 * @param entry Entry point called with context on the process's own stack.
 * @param context Opaque pointer passed to entry.
 */
int app_runner__spawn_loader_process(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context
);

/**
 * @brief Variant that transfers ownership of context to Core.
 *
 * cleanup runs exactly once after normal return, force-kill, or
 * cancellation before entry starts.
 *
 * @param permission_key Permission-check key for the new process, e.g. "game.elf".
 * @param gui_requested Whether the new process should be a GUI process.
 * @param mode Whether to launch in the foreground or background.
 * @param stack_size Stack size in bytes for the new process, or 0 for the Core default.
 * @param environment Extra environment variables to give the process.
 * @param environment_count Number of entries in environment.
 * @param entry Entry point called with context on the process's own stack.
 * @param context Opaque pointer passed to entry and cleanup; ownership transfers to Core.
 * @param cleanup Called exactly once to release context.
 */
int app_runner__spawn_loader_process_owned(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context, bruce_loader_process_cleanup_fn cleanup
);

/**
 * @brief Owned variant for runtimes which must be explicitly interrupted to observe cooperative INT/TERM.
 *
 * (for example, a compute-bound WebAssembly VM).
 *
 * @param permission_key Permission-check key for the new process, e.g. "game.elf".
 * @param gui_requested Whether the new process should be a GUI process.
 * @param mode Whether to launch in the foreground or background.
 * @param stack_size Stack size in bytes for the new process, or 0 for the Core default.
 * @param environment Extra environment variables to give the process.
 * @param environment_count Number of entries in environment.
 * @param entry Entry point called with context on the process's own stack.
 * @param context Opaque pointer passed to entry, cleanup, and stop; ownership transfers to Core.
 * @param cleanup Called exactly once to release context.
 * @param stop Non-blocking hook invoked when INT/TERM is delivered.
 */
int app_runner__spawn_loader_process_owned_with_stop(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context, bruce_loader_process_cleanup_fn cleanup,
    bruce_loader_process_stop_fn stop
);
