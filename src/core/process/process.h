#pragma once

/* Core-internal process/runtime registry.  Built-in modules and apps must never
 * include this header; they use only "core_sdk/process.h" (and, for tracked
 * memory, "core_sdk/memory.h").  This header is for AppRunner and other Core
 * services (memory, storage, dialog, ...) that need to create processes or
 * register resources for automatic cleanup. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"

/* Cleanup callback invoked automatically, in reverse-registration order, when
 * the owning process exits or is killed without releasing the resource itself.
 * It must not block and should not itself call process_registry__* for a
 * *different* process. */
typedef void (*bruce_process_resource_cleanup_t)(void *context);

typedef struct {
    /* Display/log name; copied, may be NULL (-> "app"). */
    const char *name;
    /* Entry point matching the built-in app_main signature. Exactly one of
     * `entry` or `process_entry` below must be non-NULL. */
    bruce_app_entry_t entry;
    int argc;
    /* Shallow array of pointers; the strings and the array are deep-copied
     * for the process's lifetime, so the caller may free its own copy right
     * after process_registry__create() returns. */
    char **argv;
    bool built_in;
    /* Recorded on the process record immediately, before the process's first
     * instruction runs (this is the "GUI environment context" AppRunner records
     * ahead of any launch-time permission check). */
    bool gui_requested;
    /* Filename with extension (e.g. "game.elf"), used as the
     * permission__* lookup key for this process. Ignored for built_in processes,
     * which are always granted every permission regardless of this field.
     * NULL or empty means "no permission key": permission__check() denies
     * every permission for such an external process. The ELF/JS/WASM loaders
     * (Stage 3 / A6-A7) are expected to pass the launched file's basename
     * here. */
    const char *permission_key;
    /* The new process is BRUCE_PROCESS_STARTING until it actually begins running;
     * at that point (still before its entry point is called) it transitions
     * itself: false => pushed onto the foreground stack as
     * BRUCE_PROCESS_FOREGROUND, displacing the current top; true => becomes
     * BRUCE_PROCESS_BACKGROUND without touching the stack. */
    bool start_in_background;
    /* Starts over the currently visible framebuffer instead of clearing it.
     * Used by system UI that renders exclusively through display overlays. */
    bool preserve_display;
    /* Temporary assignments applied after inheriting the parent's exported
     * environment. The strings are borrowed only for this call. */
    const bruce_environment_variable_t *environment;
    size_t environment_count;
    /* 0 selects a Core default (4096 bytes). */
    uint32_t stack_bytes;
    /* 0 selects a Core default (tskIDLE_PRIORITY + 1, the FreeRTOS priority
     * every process ran at before this field existed). Set explicitly only
     * for Core-owned worker pools (e.g. http_server's async workers) that
     * need to keep a specific FreeRTOS priority across the switch to being
     * process_registry-tracked. */
    uint32_t priority;
    /* Alternative entry points used by loader modules via
     * app_runner__spawn_loader_process() (see core_sdk/ext_mem_loader.h) instead of
     * `entry` above: called as process_entry(process_entry_context) on the new
     * process's own stack, with no argc/argv handling of its own - a loader
     * hands its own decoded image/context through process_entry_context.
     * Exactly one entry kind must be non-NULL. */
    int (*process_entry)(void *context);
    void *process_entry_context;
    void (*process_entry_cleanup)(void *context);
    void (*process_entry_stop)(void *context, bruce_process_signal_t signal);
} process_create_params_t;

/* Creates and starts a new Core-tracked process.  Exactly one of
 * params->entry or params->process_entry must be set (see process_create_params_t).
 * On success returns BRUCE_OK and the new process's id via *out_process_id.  On
 * failure returns BRUCE_ERR_INVALID_ARGUMENT or BRUCE_ERR_NO_MEMORY (registry
 * allocation or FreeRTOS task creation failed). */
bruce_result_t
process_registry__create(const process_create_params_t *params, bruce_process_id_t *out_process_id);

/* Loads process-global environment defaults from /config/.env. Missing files
 * are created with an explanatory template. Call once after storage init. */
bool process__environment_init(void);

/* Registers a cleanup callback against the *calling* process. Returns
 * BRUCE_RESOURCE_ID_INVALID if there is no current Core process or allocation
 * fails. */
bruce_resource_id_t
process_registry__resource_register(bruce_process_resource_cleanup_t cleanup, void *context);

/* Replaces the cleanup context for a resource owned by the calling process. */
bruce_result_t process_registry__resource_update(bruce_resource_id_t resource_id, void *context);

/* Reallocates a cleanup context while holding the registry lock so process
 * teardown cannot observe a pointer that libc has moved. `caps` is a
 * heap_caps.h MALLOC_CAP_* mask routed through heap_caps_realloc(); pass 0
 * for a plain realloc() (any capability). */
void *process_registry__resource_realloc(
    bruce_resource_id_t resource_id, void *context, size_t allocation_size, uint32_t caps
);

/* Releases a resource early because the owner already cleaned it up itself
 * (e.g. an explicit storage__close()); this does NOT invoke the cleanup
 * callback again.  `resource_id` must belong to the calling process.  Returns
 * BRUCE_ERR_NOT_FOUND if it does not. */
bruce_result_t process_registry__resource_release(bruce_resource_id_t resource_id);

/* Same as process_registry__resource_release(), but only unlinks the resource
 * when its registered cleanup context is exactly `context`. Tracked allocators
 * use this to ensure damaged or stale metadata cannot release an unrelated
 * resource that happens to have the referenced ID. */
bruce_result_t
process_registry__resource_release_exact(bruce_resource_id_t resource_id, const void *context);

/* Atomically moves one resource and its memory accounting from another live
 * process to the calling process. Used when a loader prepares an image before
 * its child process exists. */
bruce_result_t process_registry__resource_transfer(
    bruce_process_id_t owner_id, bruce_resource_id_t resource_id, size_t memory_bytes, bool swap_memory,
    bruce_resource_id_t *out_resource_id
);

/* Adds (positive) or removes (negative) bytes from the calling process's
 * tracked-memory statistic.  A no-op if there is no current Core process. */
void process_registry__account_memory(int64_t delta_bytes);

/* Adds (positive) or removes (negative) bytes from the calling process's
 * tracked-swap-memory statistic (flash-backed swap allocations only).
 * A no-op if there is no current Core process. */
void process_registry__account_swap_memory(int64_t delta_bytes);

/* Prevents force-kill from deleting the calling task while it owns a Core
 * service lock. begin returns false once process shutdown has started. Calls
 * outside a managed process are accepted and end is then a no-op. */
bool process_registry__operation_begin(void);
void process_registry__operation_end(void);

/* Fills in permission-relevant context for the *calling* process: whether it is
 * built_in, its permission_key (copied, NUL-terminated, truncated to fit;
 * empty if unset), and whether it was launched with GUI=1. Any of the three
 * output pointers may be NULL to skip that field. Returns BRUCE_ERR_NOT_FOUND
 * if there is no current Core process (e.g. this runs on the boot/init process,
 * before any process_registry__create() call). Used by permission__check() and
 * the dialog__* renderer-selection logic; built-in modules and apps must
 * never call this directly. */
bruce_result_t process_registry__current_context(
    bool *out_built_in, char *out_permission_key, size_t permission_key_size, bool *out_gui_requested
);

/* Input's per-process wake channel. These helpers never run while the input mutex
 * is held except for the lock-free event-group wait itself. */
bruce_result_t process_registry__event_wake_clear(bruce_process_id_t process_id);
bruce_result_t process_registry__event_wake_wait(bruce_process_id_t process_id, uint32_t timeout_ms);
void process_registry__event_wake(bruce_process_id_t process_id);

/* Foregrounds the next background GUI process in registry order. Core services
 * use this variant because they do not execute in an app permission context. */
bruce_result_t process_registry__switch_next(void);
bruce_result_t process_registry__switch_previous(void);
/* Returns the current effective foreground process, or BRUCE_PROCESS_ID_INVALID when
 * the foreground stack is empty. */
bruce_process_id_t process_registry__foreground_id(void);

/* Marks an existing process as GUI-capable/presentable without foregrounding
 * it. Used when a process launched without GUI first attempts a real frame. */
void process_registry__mark_presentable(bruce_process_id_t process_id);

bruce_result_t process_registry__set_child_stdio_session(uint32_t session);
uint32_t process_registry__current_stdio_session(void);
