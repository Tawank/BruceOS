#pragma once

/* Core-internal task/runtime registry.  Built-in modules and apps must never
 * include this header; they use only "core_sdk/task.h" (and, for tracked
 * memory, "core_sdk/memory.h").  This header is for AppRunner and other Core
 * services (memory, storage, dialog, ...) that need to create tasks or
 * register resources for automatic cleanup. */

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"

/* Cleanup callback invoked automatically, in reverse-registration order, when
 * the owning task exits or is killed without releasing the resource itself.
 * It must not block and should not itself call task_registry__* for a
 * *different* task. */
typedef void (*bruce_task_resource_cleanup_t)(void *context);

typedef struct {
    /* Display/log name; copied, may be NULL (-> "app"). */
    const char *name;
    /* Entry point matching the built-in app_main signature. Exactly one of
     * `entry` or `task_entry` below must be non-NULL. */
    bruce_app_entry_t entry;
    int argc;
    /* Shallow array of pointers; the strings and the array are deep-copied
     * for the task's lifetime, so the caller may free its own copy right
     * after task_registry__create() returns. */
    char **argv;
    bool built_in;
    /* Recorded on the task record immediately, before the task's first
     * instruction runs (this is the "--gui task context" AppRunner records
     * ahead of any launch-time permission check). */
    bool gui_requested;
    /* Filename with extension (e.g. "game.elf"), used as the
     * permission__* lookup key for this task. Ignored for built_in tasks,
     * which are always granted every permission regardless of this field.
     * NULL or empty means "no permission key": permission__check() denies
     * every permission for such an external task. The ELF/JS loaders
     * (Stage 3 / A6-A7) are expected to pass the launched file's basename
     * here. */
    const char *permission_key;
    /* The new task is BRUCE_TASK_STARTING until it actually begins running;
     * at that point (still before its entry point is called) it transitions
     * itself: false => pushed onto the foreground stack as
     * BRUCE_TASK_FOREGROUND, displacing the current top; true => becomes
     * BRUCE_TASK_BACKGROUND without touching the stack. */
    bool start_in_background;
    /* 0 selects a Core default (4096 bytes). */
    uint32_t stack_bytes;
    /* Alternative entry point used by loader modules via
     * app_runner__spawn_loader_task() (see core_sdk/loader.h) instead of
     * `entry` above: called as task_entry(task_entry_context) on the new
     * task's own stack, with no argc/argv handling of its own - a loader
     * hands its own decoded image/context through task_entry_context.
     * Exactly one of `entry` or `task_entry` must be non-NULL. */
    void (*task_entry)(void *context);
    void *task_entry_context;
} task_create_params_t;

/* Creates and starts a new Core-tracked task.  Exactly one of
 * params->entry or params->task_entry must be set (see task_create_params_t).
 * On success returns BRUCE_OK and the new task's id via *out_task_id.  On
 * failure returns BRUCE_ERR_INVALID_ARGUMENT, BRUCE_ERR_RESOURCE_LIMIT (task
 * table full), or BRUCE_ERR_NO_MEMORY (FreeRTOS task creation failed). */
bruce_result_t task_registry__create(const task_create_params_t *params, bruce_task_id_t *out_task_id);

/* Registers a cleanup callback against the *calling* task.  Returns
 * BRUCE_RESOURCE_ID_INVALID if there is no current Core task or the task's
 * resource table is full. */
bruce_resource_id_t task_registry__resource_register(bruce_task_resource_cleanup_t cleanup, void *context);

/* Releases a resource early because the owner already cleaned it up itself
 * (e.g. an explicit storage__close()); this does NOT invoke the cleanup
 * callback again.  `resource_id` must belong to the calling task.  Returns
 * BRUCE_ERR_NOT_FOUND if it does not. */
bruce_result_t task_registry__resource_release(bruce_resource_id_t resource_id);

/* Adds (positive) or removes (negative) bytes from the calling task's
 * tracked-memory statistic.  A no-op if there is no current Core task. */
void task_registry__account_memory(int64_t delta_bytes);

/* Fills in permission-relevant context for the *calling* task: whether it is
 * built_in, its permission_key (copied, NUL-terminated, truncated to fit;
 * empty if unset), and whether it was launched with --gui. Any of the three
 * output pointers may be NULL to skip that field. Returns BRUCE_ERR_NOT_FOUND
 * if there is no current Core task (e.g. this runs on the boot/init task,
 * before any task_registry__create() call). Used by permission__check() and
 * the dialog__* renderer-selection logic; built-in modules and apps must
 * never call this directly. */
bruce_result_t task_registry__current_context(bool *out_built_in, char *out_permission_key,
                                               size_t permission_key_size, bool *out_gui_requested);
