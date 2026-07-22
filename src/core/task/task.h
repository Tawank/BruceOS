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
    /* Required entry point, matching the ELF/built-in app_main signature. */
    bruce_app_entry_t entry;
    int argc;
    /* Shallow array of pointers; the strings and the array are deep-copied
     * for the task's lifetime, so the caller may free its own copy right
     * after task_registry__create() returns. */
    char **argv;
    bool built_in;
    bool gui_requested;
    /* false => the new task is pushed onto the foreground stack and starts
     * BRUCE_TASK_FOREGROUND, displacing the current top; true => it starts
     * BRUCE_TASK_BACKGROUND without touching the stack. */
    bool start_in_background;
    /* 0 selects a Core default (4096 bytes). */
    uint32_t stack_bytes;
} task_create_params_t;

/* Creates and starts a new Core-tracked task.  On success returns BRUCE_OK
 * and the new task's id via *out_task_id.  On failure returns
 * BRUCE_ERR_INVALID_ARGUMENT, BRUCE_ERR_RESOURCE_LIMIT (task table full), or
 * BRUCE_ERR_NO_MEMORY (FreeRTOS task creation failed). */
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
