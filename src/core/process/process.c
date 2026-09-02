#include "process_internal.h"

#include "core/display/display.h"
#include "core/event_loop/event_loop.h"
#include "core/memory/memory.h"
#include "core/stdio/stdio.h"
#include "core_sdk/display.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PROCESS__DEFAULT_STACK_BYTES 4096u
/* ESP-IDF pthread reserves slot 0 in pthread_local_storage.c. */
#if CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS <= PROCESS__TLS_SLOT
#error "Bruce Core requires a dedicated FreeRTOS TLS pointer after the pthread slot"
#endif

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

process__record_t *s_processes;
process__record_t *s_process_tail;
static process__completion_t *s_completions;
static bruce_process_id_t s_next_process_id = 1;
static uint64_t s_next_completion_sequence = 1;

static process__record_t *s_fg_head;
process__record_t *s_fg_tail;
bruce_process_id_t s_effective_foreground;

static uint32_t s_last_total_runtime;
process__environment_t s_global_environment;

#define PROCESS__REAP_STACK_BYTES 2048u

/* One node per statically-created task waiting for its stack/TCB buffers to
 * be freed once FreeRTOS confirms it will never run again (see
 * process__enqueue_reap_locked() and process__reap_task() below). */
typedef struct process__reap_entry {
    TaskHandle_t handle;
    void *stack_buffer;
    void *tcb_buffer;
    struct process__reap_entry *next;
} process__reap_entry_t;

static process__reap_entry_t *s_reap_head;
static process__reap_entry_t *s_reap_tail;
static TaskHandle_t s_reaper_handle;

/* Frees a statically-created task's stack/TCB buffers directly. Safe only
 * once FreeRTOS guarantees the task will never run again - e.g. right after
 * vTaskDelete() on a *different*, still-live task, which takes effect
 * immediately. Self-deleting tasks must go through process__enqueue_reap_locked()
 * instead. */
void process__free_stack_buffers(void *stack_buffer, void *tcb_buffer) {
    memory__header_t *header = ((memory__header_t *)stack_buffer) - 1;
    header->magic = 0;
    memory__stack_free(header);
    free(tcb_buffer);
}

/* Persistent background task (not a Bruce process itself) that reclaims
 * self-deleted tasks' buffers. A task can never free its own stack while
 * still executing on it, so process__trampoline() and process__kill()'s
 * self-kill path suspend themselves (vTaskSuspend(NULL), never resumed)
 * instead of self-deleting, and hand their buffers off here.
 *
 * eTaskGetState() == eDeleted is deliberately not used to detect this: a
 * self-deleted task reports eDeleted the instant it is queued for the idle
 * task's deferred cleanup, before that cleanup has actually run - freeing its
 * buffers that early races the idle task's own list bookkeeping. eSuspended
 * carries no such caveat, and once suspended (and never resumed) the task is
 * guaranteed to never touch its stack again, so deleting it from here (not
 * self, not currently running) completes synchronously with no idle-task
 * involvement, just like killing any other suspended task. */
static void process__reap_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            process__lock();
            process__reap_entry_t *entry = s_reap_head;
            if (entry != NULL) {
                s_reap_head = entry->next;
                if (s_reap_head == NULL) s_reap_tail = NULL;
            }
            process__unlock();
            if (entry == NULL) break;

            while (eTaskGetState(entry->handle) != eSuspended) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            vTaskDelete(entry->handle);
            process__free_stack_buffers(entry->stack_buffer, entry->tcb_buffer);
            free(entry);
        }
    }
}

void process__enqueue_reap_locked(TaskHandle_t handle, void *stack_buffer, void *tcb_buffer) {
    process__reap_entry_t *entry = malloc(sizeof(*entry));
    if (entry == NULL) {
        /* A process already committed to exiting must not block or fail
         * because of this: leak the two buffers instead. Vanishingly rare,
         * and no worse than the exiting process continuing to hold them. */
        return;
    }
    entry->handle = handle;
    entry->stack_buffer = stack_buffer;
    entry->tcb_buffer = tcb_buffer;
    entry->next = NULL;
    if (s_reap_tail != NULL) s_reap_tail->next = entry;
    else s_reap_head = entry;
    s_reap_tail = entry;
    if (s_reaper_handle != NULL) xTaskNotifyGive(s_reaper_handle);
}

void process__ensure_init(void) {
    if (s_lock != NULL) { return; }
    bool created = false;
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
        created = true;
    }
    portEXIT_CRITICAL(&s_init_mux);
    if (!created) return;
    xTaskCreate(
        process__reap_task, "reaper", PROCESS__REAP_STACK_BYTES, NULL, tskIDLE_PRIORITY + 1, &s_reaper_handle
    );
}

void process__lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }

void process__unlock(void) { xSemaphoreGiveRecursive(s_lock); }

/* Caller must hold the lock. */
process__record_t *process__find_by_id_locked(bruce_process_id_t id) {
    if (id == BRUCE_PROCESS_ID_INVALID) { return NULL; }
    for (process__record_t *record = s_processes; record != NULL; record = record->next) {
        if (record->in_use && record->id == id) { return record; }
    }
    return NULL;
}

/* Caller must hold the lock. */
process__record_t *process__find_by_handle_locked(TaskHandle_t handle) {
    if (handle == NULL) { return NULL; }
    for (process__record_t *record = s_processes; record != NULL; record = record->next) {
        if (record->in_use && record->handle == handle) { return record; }
    }
    return NULL;
}

/* Caller must hold the lock. */
process__completion_t *process__find_completion_locked(bruce_process_id_t id) {
    for (process__completion_t *completion = s_completions; completion != NULL;
         completion = completion->next) {
        if (completion->in_use && completion->id == id) { return completion; }
    }
    return NULL;
}

/* Caller must hold the lock. */
static bool process__id_exists_locked(bruce_process_id_t id) {
    return process__find_by_id_locked(id) != NULL || process__find_completion_locked(id) != NULL;
}

/* IDs cross public APIs as positive int results, so reserve zero and values
 * above INT_MAX while also avoiding retained completions after wraparound. */
static bruce_process_id_t process__allocate_id_locked(void) {
    for (;;) {
        bruce_process_id_t candidate = s_next_process_id;
        s_next_process_id = candidate >= (bruce_process_id_t)INT_MAX ? 1 : candidate + 1;
        if (!process__id_exists_locked(candidate)) { return candidate; }
    }
}

void process__wake_locked(process__record_t *record) {
    xEventGroupSetBits(record->events, PROCESS__EVT_WAKE);
    if (record->wait_attached) { xEventGroupSetBits(record->wait_target->events, PROCESS__EVT_WAITER_WAKE); }
}

/* Caller must hold the lock. Dead records remain until all waiters have
 * released their event-group references. */
void process__dispose_if_unused_locked(process__record_t *record) {
    if (record->in_use || record->waiter_count != 0) return;
    if (record->previous != NULL) record->previous->next = record->next;
    else s_processes = record->next;
    if (record->next != NULL) record->next->previous = record->previous;
    else s_process_tail = record->previous;
    vEventGroupDelete(record->events);
    free(record);
}

/* Caller must hold the lock. Pinned completions allocate independently so a
 * large set of simultaneous status waiters cannot prevent process teardown. */
static void
process__publish_completion_locked(process__record_t *record, const bruce_process_status_t *status) {
    process__completion_t *target = NULL;
    for (process__completion_t *completion = s_completions; completion != NULL;
         completion = completion->next) {
        if (!completion->in_use) {
            target = completion;
            break;
        }
        if (completion->waiter_pins == 0 && (target == NULL || completion->sequence < target->sequence)) {
            target = completion;
        }
    }
    if (target == NULL) {
        target = calloc(1, sizeof(*target));
        if (target == NULL) return;
        target->next = s_completions;
        s_completions = target;
    }

    target->in_use = true;
    target->id = record->id;
    target->status = *status;
    target->sequence = s_next_completion_sequence++;
    target->waiter_pins = record->status_waiter_count;
}

void process__completion_clear_locked(process__completion_t *completion) {
    completion->in_use = false;
    completion->id = BRUCE_PROCESS_ID_INVALID;
    memset(&completion->status, 0, sizeof(completion->status));
    completion->sequence = 0;
    completion->waiter_pins = 0;
}

/* Releases a wait owned by a tracked process. This is also called from forced
 * teardown because deleting a blocked task prevents process__wait_common()
 * from releasing its own pin. */
void process__detach_wait_locked(process__record_t *waiter) {
    if (!waiter->wait_attached) return;
    process__record_t *target = waiter->wait_target;
    process__completion_t *completion = process__find_completion_locked(target->id);
    if (waiter->wait_for_status && completion != NULL && completion->waiter_pins > 0) {
        completion->waiter_pins--;
    }
    if (target->waiter_count > 0) target->waiter_count--;
    if (waiter->wait_for_status && target->status_waiter_count > 0) target->status_waiter_count--;
    waiter->wait_attached = false;
    waiter->wait_target = NULL;
    process__dispose_if_unused_locked(target);
}

/* Removes `id` from the foreground stack if (and only if) it is currently on
 * top, restoring the process beneath it (if any) to BRUCE_PROCESS_FOREGROUND.
 * Caller must hold the lock. */
static void process__foreground_notify_locked(bruce_process_id_t previous, bruce_process_id_t current) {
    event_loop__foreground_changed(current);
    if (previous != BRUCE_PROCESS_ID_INVALID) {
        process__record_t *record = process__find_by_id_locked(previous);
        if (record != NULL) { xEventGroupSetBits(record->events, PROCESS__EVT_EVENT_WAKE); }
    }
    if (current != BRUCE_PROCESS_ID_INVALID) {
        process__record_t *record = process__find_by_id_locked(current);
        if (record != NULL) { xEventGroupSetBits(record->events, PROCESS__EVT_EVENT_WAKE); }
    }
}

/* Compact the stack and derive all runnable foreground/background states from
 * it. Paused processes retain their position but are temporarily ineligible. */
void process__foreground_recompute_locked(void) {
    bruce_process_id_t next = BRUCE_PROCESS_ID_INVALID;
    for (process__record_t *record = s_fg_head; record != NULL;) {
        process__record_t *following = record->fg_next;
        if (!record->in_use || record->state == BRUCE_PROCESS_STOPPING) {
            if (record->fg_previous != NULL) record->fg_previous->fg_next = record->fg_next;
            else s_fg_head = record->fg_next;
            if (record->fg_next != NULL) record->fg_next->fg_previous = record->fg_previous;
            else s_fg_tail = record->fg_previous;
            record->fg_previous = NULL;
            record->fg_next = NULL;
        } else if (record->state != BRUCE_PROCESS_PAUSED && record->state != BRUCE_PROCESS_STARTING) {
            next = record->id;
        }
        record = following;
    }

    for (process__record_t *record = s_processes; record != NULL; record = record->next) {
        if (!record->in_use || record->state == BRUCE_PROCESS_STARTING ||
            record->state == BRUCE_PROCESS_PAUSED || record->state == BRUCE_PROCESS_STOPPING) {
            continue;
        }
        bruce_process_state_t new_state =
            record->id == next ? BRUCE_PROCESS_FOREGROUND : BRUCE_PROCESS_BACKGROUND;
        if (record->state != new_state) {
            record->state = new_state;
            process__wake_locked(record);
            display__process_state_changed(record->id, new_state);
        }
    }

    if (next != s_effective_foreground) {
        bruce_process_id_t previous = s_effective_foreground;
        s_effective_foreground = next;
        process__foreground_notify_locked(previous, next);
    }
}

void process__foreground_remove_locked(bruce_process_id_t id) {
    process__record_t *record = process__find_by_id_locked(id);
    if (record == NULL || (record->fg_previous == NULL && record->fg_next == NULL && s_fg_head != record))
        return;
    if (record->fg_previous != NULL) record->fg_previous->fg_next = record->fg_next;
    else s_fg_head = record->fg_next;
    if (record->fg_next != NULL) record->fg_next->fg_previous = record->fg_previous;
    else s_fg_tail = record->fg_previous;
    record->fg_previous = NULL;
    record->fg_next = NULL;
}

void process__foreground_push_locked(bruce_process_id_t id) {
    process__foreground_remove_locked(id);
    process__record_t *record = process__find_by_id_locked(id);
    if (record != NULL) {
        record->fg_previous = s_fg_tail;
        if (s_fg_tail != NULL) s_fg_tail->fg_next = record;
        else s_fg_head = record;
        s_fg_tail = record;
        record->presentable = true;
    }
    process__foreground_recompute_locked();
}

void process__refresh_cpu_samples_locked(void) {
    const size_t status_capacity = uxTaskGetNumberOfTasks();
    TaskStatus_t *status_buf = malloc(status_capacity * sizeof(*status_buf));
    if (status_buf == NULL) return;
    uint32_t total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(status_buf, status_capacity, &total_runtime);
    uint32_t total_delta = total_runtime - s_last_total_runtime;

    for (process__record_t *record = s_processes; record != NULL; record = record->next) {
        if (!record->in_use || record->handle == NULL) { continue; }
        for (UBaseType_t j = 0; j < count; ++j) {
            if (status_buf[j].xHandle == record->handle) {
                uint32_t delta = status_buf[j].ulRunTimeCounter - record->last_runtime_counter;
                record->cpu_percent =
                    total_delta > 0 ? (uint32_t)(((uint64_t)delta * 100u) / total_delta) : 0u;
                record->last_runtime_counter = status_buf[j].ulRunTimeCounter;
                record->stack_high_water_bytes =
                    (uint32_t)(uxTaskGetStackHighWaterMark(record->handle) * sizeof(StackType_t));
                break;
            }
        }
    }
    s_last_total_runtime = total_runtime;
    free(status_buf);
}

void process__fill_snapshot_locked(const process__record_t *record, bruce_process_snapshot_t *out_snapshot) {
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->id = record->id;
    out_snapshot->state = record->state;
    strncpy(out_snapshot->name, record->name, BRUCE_PROCESS_NAME_MAX - 1);
    out_snapshot->stack_high_water_bytes = record->stack_high_water_bytes;
    out_snapshot->stack_total_bytes = record->stack_total_bytes;
    out_snapshot->cpu_percent = record->cpu_percent;
    out_snapshot->memory_bytes = record->memory_bytes;
    out_snapshot->swap_bytes = record->swap_bytes;
    out_snapshot->resource_count = record->resource_count;
    out_snapshot->built_in = record->built_in;
    out_snapshot->gui_requested = record->gui_requested;
    out_snapshot->presentable = record->presentable;
}

static void process__free_argv(int argc, char **argv) {
    if (argv == NULL) { return; }
    for (int i = 0; i < argc; ++i) { free(argv[i]); }
    free(argv);
}

static bool process__dup_argv(int argc, char *const *src_argv, char ***out_argv) {
    *out_argv = NULL;
    if (argc <= 0) { return true; }
    char **copy = calloc((size_t)argc + 1u, sizeof(char *));
    if (copy == NULL) { return false; }
    for (int i = 0; i < argc; ++i) {
        const char *source = (src_argv != NULL && src_argv[i] != NULL) ? src_argv[i] : "";
        size_t length = strlen(source) + 1;
        copy[i] = malloc(length);
        if (copy[i] == NULL) {
            process__free_argv(i, copy);
            return false;
        }
        memcpy(copy[i], source, length);
    }
    *out_argv = copy;
    return true;
}

/* Runs cleanup before atomically publishing completion and making the process
 * absent. Its dynamically allocated record remains available to waiters. */
void process__teardown_locked(process__record_t *record, const bruce_process_status_t *status) {
    if (record->stop_callback_count != 0) {
        record->teardown_pending = true;
        record->pending_status = *status;
        return;
    }
    process__detach_wait_locked(record);
    while (record->resources != NULL) {
        process__resource_t *resource = record->resources;
        record->resources = resource->next;
        if (resource->cleanup != NULL) { resource->cleanup(resource->context); }
        free(resource);
    }
    record->resource_count = 0;
    record->memory_bytes = 0;
    record->swap_bytes = 0;
    if (record->process_entry_cleanup != NULL) {
        record->process_entry_cleanup(record->process_entry_context);
        record->process_entry_context = NULL;
    }

    process__foreground_remove_locked(record->id);
    display__process_removed(record->id);
    process__foreground_recompute_locked();
    process__free_argv(record->argc, record->argv);
    record->argv = NULL;
    if (record->handle != NULL && record->handle == xTaskGetCurrentTaskHandle()) {
        vTaskSetThreadLocalStoragePointer(record->handle, PROCESS__TLS_SLOT, NULL);
    }
    process__environment_free(&record->environment);

    process__publish_completion_locked(record, status);
    record->in_use = false;
    record->handle = NULL;
    xEventGroupSetBits(record->events, PROCESS__EVT_EXITED);
    process__dispose_if_unused_locked(record);
}

static void process__trampoline(void *arg) {
    process__record_t *record = (process__record_t *)arg;
    FILE *stdio_input = NULL;
    FILE *stdio_output = NULL;
    FILE *stdio_error = NULL;

    vTaskSetThreadLocalStoragePointer(NULL, PROCESS__TLS_SLOT, record);

    /* The record was created in BRUCE_PROCESS_STARTING; this is the first thing
     * the new process does once FreeRTOS actually schedules it, and still runs
     * before record->entry() sees a single instruction. */
    process__lock();
    if (record->stop_requested) {
        bruce_process_status_t status = {
            .reason = BRUCE_PROCESS_TERMINATED,
            .exit_code = 0,
            .signal = record->pending_signal,
        };
        /* Read before teardown, which may free `record` itself. */
        void *stack_buffer = record->stack_buffer;
        void *tcb_buffer = record->tcb_buffer;
        process__teardown_locked(record, &status);
        process__enqueue_reap_locked(xTaskGetCurrentTaskHandle(), stack_buffer, tcb_buffer);
        process__unlock();
        vTaskSuspend(NULL);
        return;
    }
    process__unlock();

    /* The display context is marked clear_on_next_frame when this process is
     * promoted to the foreground. Keep the previous process's completed panel
     * frame visible while application code initializes; the new process's
     * first display__begin_frame() clears its framebuffer, and its first
     * display__present() replaces the old UI atomically. */

    stdio__process_attach(record->stdio_session, &stdio_input, &stdio_output, &stdio_error);

    int exit_code = 0;
    if (record->process_entry != NULL) {
        exit_code = record->process_entry(record->process_entry_context);
    } else if (record->entry != NULL) {
        exit_code = record->entry(record->argc, record->argv);
    }

    stdio__process_detach(stdio_input, stdio_output, stdio_error);

    process__lock();
    if (!record->in_use) {
        process__enqueue_reap_locked(xTaskGetCurrentTaskHandle(), record->stack_buffer, record->tcb_buffer);
        process__unlock();
        vTaskSuspend(NULL);
        return;
    }
    bruce_process_status_t status = {
        .reason = record->stop_requested ? BRUCE_PROCESS_TERMINATED : BRUCE_PROCESS_EXITED,
        .exit_code = record->stop_requested ? 0 : exit_code,
        .signal = record->stop_requested ? record->pending_signal : (bruce_process_signal_t)0,
    };
    /* Read before teardown, which may free `record` itself. */
    void *stack_buffer = record->stack_buffer;
    void *tcb_buffer = record->tcb_buffer;
    process__teardown_locked(record, &status);
    process__enqueue_reap_locked(xTaskGetCurrentTaskHandle(), stack_buffer, tcb_buffer);
    process__unlock();

    vTaskSuspend(NULL);
}

bruce_result_t
process_registry__create(const process_create_params_t *params, bruce_process_id_t *out_process_id) {
    process__ensure_init();
    if (params == NULL || out_process_id == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    unsigned entry_count = (params->entry != NULL) + (params->process_entry != NULL);
    if (entry_count != 1) {
        /* exactly one entry kind must be set */
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_process_id = BRUCE_PROCESS_ID_INVALID;

    char **argv_copy = NULL;
    if (!process__dup_argv(params->argc, params->argv, &argv_copy)) { return BRUCE_ERR_NO_MEMORY; }

    process__lock();
    process__record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        process__unlock();
        process__free_argv(params->argc, argv_copy);
        return BRUCE_ERR_NO_MEMORY;
    }
    record->events = xEventGroupCreate();
    if (record->events == NULL) {
        free(record);
        process__unlock();
        process__free_argv(params->argc, argv_copy);
        return BRUCE_ERR_NO_MEMORY;
    }
    xEventGroupSetBits(record->events, PROCESS__EVT_OPERATION_IDLE | PROCESS__EVT_STOP_CALLBACK_IDLE);
    record->previous = s_process_tail;
    if (s_process_tail != NULL) s_process_tail->next = record;
    else s_processes = record;
    s_process_tail = record;
    record->in_use = true;
    record->id = process__allocate_id_locked();
    process__record_t *parent = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (parent != NULL) {
        record->stdio_session = parent->child_stdio_session;
        /* A routed shell must pass the same terminal session to commands it
         * launches, not only use it for the shell's own stdin/stdout. */
        record->child_stdio_session = parent->child_stdio_session;
    }
    bruce_result_t environment_result =
        process__environment_inherit_locked(record, parent, params->environment, params->environment_count);
    if (environment_result != BRUCE_OK) {
        process__environment_free(&record->environment);
        record->in_use = false;
        process__dispose_if_unused_locked(record);
        process__unlock();
        process__free_argv(params->argc, argv_copy);
        return environment_result;
    }
    strncpy(
        record->name,
        params->name != NULL && params->name[0] != '\0' ? params->name : "app",
        BRUCE_PROCESS_NAME_MAX - 1
    );
    record->built_in = params->built_in;
    record->gui_requested = params->gui_requested;
    if (params->permission_key != NULL && params->permission_key[0] != '\0') {
        strncpy(record->permission_key, params->permission_key, BRUCE_PERMISSION_FILE_NAME_MAX - 1);
        record->permission_key[BRUCE_PERMISSION_FILE_NAME_MAX - 1] = '\0';
    }
    record->start_in_background = params->start_in_background;
    record->preserve_display = params->preserve_display;
    record->entry = params->entry;
    record->process_entry = params->process_entry;
    record->process_entry_context = params->process_entry_context;
    record->process_entry_cleanup = params->process_entry_cleanup;
    record->process_entry_stop = params->process_entry_stop;
    record->argc = params->argc > 0 ? params->argc : 0;
    record->argv = argv_copy;
    record->next_resource_id = 1;
    uint32_t stack_bytes = params->stack_bytes != 0 ? params->stack_bytes : PROCESS__DEFAULT_STACK_BYTES;
    record->stack_total_bytes = stack_bytes;

    /* The stack is allocated (and freed - see process__free_stack_buffers())
     * by Bruce itself rather than FreeRTOS, so it carries a Bruce tracked-
     * memory header and shows up in `free -m` as the process's own memory
     * instead of an anonymous heap block. It is deliberately never registered
     * via process_registry__resource_register(): process__teardown_locked()
     * runs its resource cleanups synchronously while the process is still
     * executing (before it self-deletes), which would free a running task's
     * own stack out from under it.
     *
     * Where those bytes physically come from (general heap vs. RTC memory)
     * is memory.c's call, not this function's - see memory__stack_alloc(). */
    size_t stack_total = sizeof(memory__header_t) + stack_bytes;
    memory__header_t *stack_header = memory__stack_alloc(stack_total);
    StaticTask_t *tcb_buffer = stack_header != NULL ? malloc(sizeof(StaticTask_t)) : NULL;
    if (stack_header == NULL || tcb_buffer == NULL) {
        memory__stack_free(stack_header);
        free(tcb_buffer);
        process__free_argv(record->argc, record->argv);
        process__environment_free(&record->environment);
        record->in_use = false;
        process__dispose_if_unused_locked(record);
        process__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    stack_header->magic = MEMORY__MAGIC;
    stack_header->size = stack_bytes;
    stack_header->resource_id = record->next_resource_id++;
    stack_header->owner_id = record->id;
    stack_header->is_stack = true;
    /* memory__stack_alloc() never hands back memory_rtc.c's hand-rolled
     * pool (a stack pointer can't live there - see its comment), so this is
     * always false; memory__stack_free() doesn't inspect it either way. */
    stack_header->is_rtc_pool = false;
    record->stack_buffer = (void *)(stack_header + 1);
    record->tcb_buffer = tcb_buffer;
    record->memory_bytes += stack_bytes;

    UBaseType_t priority = params->priority != 0 ? (UBaseType_t)params->priority : tskIDLE_PRIORITY + 1;
    record->handle = xTaskCreateStatic(
        process__trampoline, record->name, stack_bytes, record, priority, (StackType_t *)record->stack_buffer,
        tcb_buffer
    );
    if (record->handle == NULL) {
        process__free_stack_buffers(record->stack_buffer, record->tcb_buffer);
        record->stack_buffer = NULL;
        record->tcb_buffer = NULL;
        process__free_argv(record->argc, record->argv);
        process__environment_free(&record->environment);
        record->in_use = false;
        process__dispose_if_unused_locked(record);
        process__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    display__process_created(record->id, record->gui_requested);
    record->state = BRUCE_PROCESS_BACKGROUND;
    if (record->start_in_background) {
        display__process_state_changed(record->id, record->state);
    } else {
        process__foreground_push_locked(record->id);
    }
    *out_process_id = record->id;
    process__unlock();
    return BRUCE_OK;
}

process__record_t *process__current_record(void) {
    return (process__record_t *)pvTaskGetThreadLocalStoragePointer(NULL, PROCESS__TLS_SLOT);
}

bruce_resource_id_t
process_registry__resource_register(bruce_process_resource_cleanup_t cleanup, void *context) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_RESOURCE_ID_INVALID;
    }
    process__resource_t *resource = malloc(sizeof(*resource));
    if (resource == NULL) {
        process__unlock();
        return BRUCE_RESOURCE_ID_INVALID;
    }
    bruce_resource_id_t id = self->next_resource_id++;
    if (self->next_resource_id == BRUCE_RESOURCE_ID_INVALID) { self->next_resource_id = 1; }
    *resource =
        (process__resource_t){.id = id, .cleanup = cleanup, .context = context, .next = self->resources};
    self->resources = resource;
    self->resource_count++;
    process__unlock();
    return id;
}

bruce_result_t process_registry__resource_update(bruce_resource_id_t resource_id, void *context) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    for (process__resource_t *resource = self->resources; resource != NULL; resource = resource->next) {
        if (resource->id == resource_id) {
            resource->context = context;
            process__unlock();
            return BRUCE_OK;
        }
    }
    process__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

void *process_registry__resource_realloc(
    bruce_resource_id_t resource_id, void *context, size_t allocation_size, uint32_t caps
) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return NULL;
    }
    for (process__resource_t *resource = self->resources; resource != NULL; resource = resource->next) {
        if (resource->id == resource_id && resource->context == context) {
            void *resized = caps != 0 ? heap_caps_realloc(context, allocation_size, caps)
                                       : realloc(context, allocation_size);
            if (resized != NULL) resource->context = resized;
            process__unlock();
            return resized;
        }
    }
    process__unlock();
    return NULL;
}

static bruce_result_t
process__resource_release(bruce_resource_id_t resource_id, const void *context, bool match_context) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    process__resource_t **link = &self->resources;
    while (*link != NULL) {
        process__resource_t *resource = *link;
        if (resource->id == resource_id && (!match_context || resource->context == context)) {
            *link = resource->next;
            free(resource);
            self->resource_count--;
            process__unlock();
            return BRUCE_OK;
        }
        link = &resource->next;
    }
    process__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

bruce_result_t process_registry__resource_release(bruce_resource_id_t resource_id) {
    return process__resource_release(resource_id, NULL, false);
}

bruce_result_t
process_registry__resource_release_exact(bruce_resource_id_t resource_id, const void *context) {
    if (context == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return process__resource_release(resource_id, context, true);
}

bruce_result_t process_registry__resource_transfer(
    bruce_process_id_t owner_id, bruce_resource_id_t resource_id, size_t memory_bytes, bool swap_memory,
    bruce_resource_id_t *out_resource_id
) {
    if (owner_id == BRUCE_PROCESS_ID_INVALID || resource_id == BRUCE_RESOURCE_ID_INVALID ||
        out_resource_id == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    process__ensure_init();
    process__lock();
    process__record_t *owner = process__find_by_id_locked(owner_id);
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (owner == NULL || self == NULL || owner == self) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }

    process__resource_t **source_link = &owner->resources;
    while (*source_link != NULL && (*source_link)->id != resource_id) source_link = &(*source_link)->next;
    if (*source_link == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }

    bruce_resource_id_t new_id = self->next_resource_id++;
    if (self->next_resource_id == BRUCE_RESOURCE_ID_INVALID) self->next_resource_id = 1;
    process__resource_t *resource = *source_link;
    *source_link = resource->next;
    resource->id = new_id;
    resource->next = self->resources;
    self->resources = resource;
    owner->resource_count--;
    self->resource_count++;
    if (memory_bytes <= owner->memory_bytes) owner->memory_bytes -= memory_bytes;
    else owner->memory_bytes = 0;
    self->memory_bytes += memory_bytes;
    if (swap_memory) {
        if (memory_bytes <= owner->swap_bytes) owner->swap_bytes -= memory_bytes;
        else owner->swap_bytes = 0;
        self->swap_bytes += memory_bytes;
    }
    *out_resource_id = new_id;
    process__unlock();
    return BRUCE_OK;
}

void process_registry__account_memory(int64_t delta_bytes) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self != NULL) {
        if (delta_bytes >= 0 || (size_t)(-delta_bytes) <= self->memory_bytes) {
            self->memory_bytes = (size_t)((int64_t)self->memory_bytes + delta_bytes);
        } else {
            self->memory_bytes = 0;
        }
    }
    process__unlock();
}

void process_registry__account_swap_memory(int64_t delta_bytes) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self != NULL) {
        if (delta_bytes >= 0 || (size_t)(-delta_bytes) <= self->swap_bytes) {
            self->swap_bytes = (size_t)((int64_t)self->swap_bytes + delta_bytes);
        } else {
            self->swap_bytes = 0;
        }
    }
    process__unlock();
}

bool process_registry__operation_begin(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return true;
    }
    if (self->stop_requested || self->state == BRUCE_PROCESS_STOPPING) {
        process__unlock();
        return false;
    }
    if (self->operation_count++ == 0) { xEventGroupClearBits(self->events, PROCESS__EVT_OPERATION_IDLE); }
    process__unlock();
    return true;
}

void process_registry__operation_end(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self != NULL && self->operation_count > 0 && --self->operation_count == 0) {
        xEventGroupSetBits(self->events, PROCESS__EVT_OPERATION_IDLE);
    }
    process__unlock();
}

bruce_result_t process_registry__current_context(
    bool *out_built_in, char *out_permission_key, size_t permission_key_size, bool *out_gui_requested
) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (out_built_in != NULL) { *out_built_in = self->built_in; }
    if (out_permission_key != NULL && permission_key_size > 0) {
        strncpy(out_permission_key, self->permission_key, permission_key_size - 1);
        out_permission_key[permission_key_size - 1] = '\0';
    }
    if (out_gui_requested != NULL) { *out_gui_requested = self->gui_requested; }
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process_registry__set_child_stdio_session(uint32_t session) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    /* BRUCE_STDIO_SESSION_INVALID means "stop overriding", not "route
     * children nowhere": it restores the default of routing children into
     * this process's own session, same as before any override. A caller
     * that temporarily reroutes children into a private session (a shell
     * piping to an external pager, a test harness capturing output) and
     * then calls this with INVALID to clean up must get its own routed
     * terminal back for every command it launches afterward -- not have
     * every later child silently fall back to the physical console because
     * "no session" was taken literally. */
    self->child_stdio_session = session == BRUCE_STDIO_SESSION_INVALID ? self->stdio_session : session;
    process__unlock();
    return BRUCE_OK;
}

uint32_t process_registry__current_stdio_session(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    uint32_t session = self != NULL ? self->stdio_session : BRUCE_STDIO_SESSION_INVALID;
    process__unlock();
    return session;
}

/* Temporarily overrides the calling process's *own* stdio_session (not its
 * children's -- that's set_child_stdio_session() above): pushes the current
 * value onto stdio_session_stack and installs `session` in its place, so a
 * process's own stdio__printf()/stdio__write() calls -- not just the
 * children it launches -- go somewhere else for a while. This is what lets
 * the shell capture a builtin's or shell function's output for ">"/">>"
 * redirection (see shell_executor__builtin_redirected()): unlike an
 * external command, a builtin/function has no separate child process whose
 * output could be relayed -- it runs in-line on the shell's own task.
 *
 * The stdio.c wrapper (stdio__session_capture_self()) ownership-checks
 * `session` the same way stdio__session_route_children() does, so this
 * never has to -- by the time a session ID reaches here, it's already been
 * established that the calling process was allowed to adopt it. */
bruce_result_t process_registry__push_own_stdio_session(uint32_t session) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (self->stdio_session_stack_depth >= PROCESS__STDIO_SESSION_STACK_MAX) {
        process__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    self->stdio_session_stack[self->stdio_session_stack_depth++] = self->stdio_session;
    self->stdio_session = session;
    process__unlock();
    return BRUCE_OK;
}

/* Undoes the most recent process_registry__push_own_stdio_session(): pops
 * and restores the value it saved. Takes no session argument at all -- by
 * design, not merely convenience -- so there is nothing here for a caller
 * to forge or guess its way into adopting a foreign session with; the value
 * restored is only ever one this exact process legitimately held before. */
bruce_result_t process_registry__pop_own_stdio_session(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (self->stdio_session_stack_depth == 0) {
        process__unlock();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    self->stdio_session = self->stdio_session_stack[--self->stdio_session_stack_depth];
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process_registry__event_wake_clear(bruce_process_id_t process_id) {
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    xEventGroupClearBits(record->events, PROCESS__EVT_EVENT_WAKE);
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process_registry__event_wake_wait(bruce_process_id_t process_id, uint32_t timeout_ms) {
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    EventGroupHandle_t events = record->events;
    process__unlock();
    TickType_t ticks = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(events, PROCESS__EVT_EVENT_WAKE, pdTRUE, pdFALSE, ticks);
    return (bits & PROCESS__EVT_EVENT_WAKE) != 0 ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
}

void process_registry__event_wake(bruce_process_id_t process_id) {
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record != NULL) { xEventGroupSetBits(record->events, PROCESS__EVT_EVENT_WAKE); }
    process__unlock();
}
