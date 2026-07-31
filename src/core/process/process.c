#include "process.h"

#include "core/display/display.h"
#include "core/input/input.h"
#include "core/stdio/stdio.h"
#include "core_sdk/display.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PROCESS__MAX_RECORDS 8
#define PROCESS__MAX_COMPLETIONS 16
#define PROCESS__MAX_RESOURCES 32
#define PROCESS__FOREGROUND_STACK_MAX PROCESS__MAX_RECORDS
#define PROCESS__DEFAULT_STACK_BYTES 4096u
#define PROCESS__EVT_WAKE (1u << 0)
#define PROCESS__EVT_EXITED (1u << 1)
#define PROCESS__EVT_INPUT_WAKE (1u << 2)
#define PROCESS__EVT_WAITER_WAKE (1u << 3)

typedef struct {
    bruce_resource_id_t id;
    bruce_process_resource_cleanup_t cleanup;
    void *context;
    bool active;
} process__resource_slot_t;

typedef struct {
    bool in_use;
    uint32_t generation;
    bruce_process_id_t id;
    char name[BRUCE_PROCESS_NAME_MAX];
    bruce_process_state_t state;
    bruce_process_state_t state_before_pause;
    bool built_in;
    bool gui_requested;
    char permission_key[BRUCE_PERMISSION_FILE_NAME_MAX];
    bool start_in_background;
    bruce_stdio_session_t stdio_session;
    bruce_stdio_session_t child_stdio_session;
    TaskHandle_t handle;

    bruce_app_entry_t entry;
    int argc;
    char **argv;

    void (*process_entry)(void *context);
    void *process_entry_context;
    volatile bool stop_requested;
    bruce_process_signal_t pending_signal;
    volatile bool pause_requested;
    size_t waiter_count;
    size_t status_waiter_count;
    bool wait_attached;
    int wait_slot;
    bool wait_for_status;

    process__resource_slot_t resources[PROCESS__MAX_RESOURCES];
    bruce_resource_id_t next_resource_id;
    size_t resource_count;
    size_t memory_bytes;

    uint32_t last_runtime_counter;
    uint32_t cpu_percent;
    uint32_t stack_high_water_bytes;
} process__record_t;

typedef struct {
    bool in_use;
    bruce_process_id_t id;
    bruce_process_status_t status;
    uint64_t sequence;
    size_t waiter_pins;
} process__completion_t;

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static process__record_t s_processes[PROCESS__MAX_RECORDS];
static process__completion_t s_completions[PROCESS__MAX_COMPLETIONS];
static EventGroupHandle_t s_process_events[PROCESS__MAX_RECORDS];
static bruce_process_id_t s_next_process_id = 1;
static uint64_t s_next_completion_sequence = 1;

static bruce_process_id_t s_fg_stack[PROCESS__FOREGROUND_STACK_MAX];
static int s_fg_depth;
static bruce_process_id_t s_effective_foreground;

static uint32_t s_last_total_runtime;

static void process__ensure_init(void) {
    if (s_lock != NULL) { return; }
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
        for (int i = 0; i < PROCESS__MAX_RECORDS; ++i) { s_process_events[i] = xEventGroupCreate(); }
    }
    portEXIT_CRITICAL(&s_init_mux);
}

static void process__lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }

static void process__unlock(void) { xSemaphoreGiveRecursive(s_lock); }

/* Caller must hold the lock. */
static process__record_t *process__find_by_id_locked(bruce_process_id_t id) {
    if (id == BRUCE_PROCESS_ID_INVALID) { return NULL; }
    for (int i = 0; i < PROCESS__MAX_RECORDS; ++i) {
        if (s_processes[i].in_use && s_processes[i].id == id) { return &s_processes[i]; }
    }
    return NULL;
}

/* Caller must hold the lock. */
static process__record_t *process__find_by_handle_locked(TaskHandle_t handle) {
    if (handle == NULL) { return NULL; }
    for (int i = 0; i < PROCESS__MAX_RECORDS; ++i) {
        if (s_processes[i].in_use && s_processes[i].handle == handle) { return &s_processes[i]; }
    }
    return NULL;
}

/* Caller must hold the lock. */
static process__completion_t *process__find_completion_locked(bruce_process_id_t id) {
    for (int i = 0; i < PROCESS__MAX_COMPLETIONS; ++i) {
        if (s_completions[i].in_use && s_completions[i].id == id) { return &s_completions[i]; }
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

static int process__slot_index_locked(const process__record_t *record) { return (int)(record - s_processes); }

static void process__wake_locked(process__record_t *record) {
    xEventGroupSetBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_WAKE);
    if (record->wait_attached) {
        xEventGroupSetBits(s_process_events[record->wait_slot], PROCESS__EVT_WAITER_WAKE);
    }
}

/* Caller must hold the lock. Completion capacity exceeds the maximum live
 * process count, so pinned entries cannot exhaust the table. */
static void process__publish_completion_locked(
    process__record_t *record, const bruce_process_status_t *status
) {
    process__completion_t *target = NULL;
    for (int i = 0; i < PROCESS__MAX_COMPLETIONS; ++i) {
        if (!s_completions[i].in_use) {
            target = &s_completions[i];
            break;
        }
        if (s_completions[i].waiter_pins == 0 &&
            (target == NULL || s_completions[i].sequence < target->sequence)) {
            target = &s_completions[i];
        }
    }

    target->in_use = true;
    target->id = record->id;
    target->status = *status;
    target->sequence = s_next_completion_sequence++;
    target->waiter_pins = record->status_waiter_count;
}

/* Releases a wait owned by a tracked process. This is also called from forced
 * teardown because deleting a blocked task prevents process__wait_common()
 * from releasing its own pin. */
static void process__detach_wait_locked(process__record_t *waiter) {
    if (!waiter->wait_attached) return;
    process__record_t *target = &s_processes[waiter->wait_slot];
    process__completion_t *completion = process__find_completion_locked(target->id);
    if (waiter->wait_for_status && completion != NULL && completion->waiter_pins > 0) {
        completion->waiter_pins--;
    }
    if (target->waiter_count > 0) target->waiter_count--;
    if (waiter->wait_for_status && target->status_waiter_count > 0) target->status_waiter_count--;
    waiter->wait_attached = false;
}

/* Removes `id` from the foreground stack if (and only if) it is currently on
 * top, restoring the process beneath it (if any) to BRUCE_PROCESS_FOREGROUND.
 * Caller must hold the lock. */
static void process__foreground_notify_locked(bruce_process_id_t previous, bruce_process_id_t current) {
    input__foreground_changed(current);
    if (previous != BRUCE_PROCESS_ID_INVALID) {
        process__record_t *record = process__find_by_id_locked(previous);
        if (record != NULL) {
            xEventGroupSetBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_INPUT_WAKE);
        }
    }
    if (current != BRUCE_PROCESS_ID_INVALID) {
        process__record_t *record = process__find_by_id_locked(current);
        if (record != NULL) {
            xEventGroupSetBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_INPUT_WAKE);
        }
    }
}

/* Compact the stack and derive all runnable foreground/background states from
 * it. Paused processes retain their position but are temporarily ineligible. */
static void process__foreground_recompute_locked(void) {
    int write = 0;
    bruce_process_id_t next = BRUCE_PROCESS_ID_INVALID;
    for (int i = 0; i < s_fg_depth; ++i) {
        process__record_t *record = process__find_by_id_locked(s_fg_stack[i]);
        if (record == NULL || record->state == BRUCE_PROCESS_STOPPING) { continue; }
        s_fg_stack[write++] = record->id;
        if (record->state != BRUCE_PROCESS_PAUSED && record->state != BRUCE_PROCESS_STARTING) { next = record->id; }
    }
    s_fg_depth = write;

    for (int i = 0; i < PROCESS__MAX_RECORDS; ++i) {
        process__record_t *record = &s_processes[i];
        if (!record->in_use || record->state == BRUCE_PROCESS_STARTING || record->state == BRUCE_PROCESS_PAUSED ||
            record->state == BRUCE_PROCESS_STOPPING) {
            continue;
        }
        bruce_process_state_t new_state = record->id == next ? BRUCE_PROCESS_FOREGROUND : BRUCE_PROCESS_BACKGROUND;
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

static void process__foreground_remove_locked(bruce_process_id_t id) {
    int write = 0;
    for (int i = 0; i < s_fg_depth; ++i) {
        if (s_fg_stack[i] != id) { s_fg_stack[write++] = s_fg_stack[i]; }
    }
    s_fg_depth = write;
}

static void process__foreground_push_locked(bruce_process_id_t id) {
    process__foreground_remove_locked(id);
    if (s_fg_depth < PROCESS__FOREGROUND_STACK_MAX) { s_fg_stack[s_fg_depth++] = id; }
    process__foreground_recompute_locked();
}

static void process__clear_foreground_display(void) {
    if (display__begin_frame() == BRUCE_OK) {
        (void)display__fill_screen(BRUCE_COLOR_BLACK);
        (void)display__present();
    }
}

static void process__refresh_cpu_samples_locked(void) {
    const size_t status_capacity = PROCESS__MAX_RECORDS + 8u;
    TaskStatus_t *status_buf = malloc(status_capacity * sizeof(*status_buf));
    if (status_buf == NULL) return;
    uint32_t total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(status_buf, status_capacity, &total_runtime);
    uint32_t total_delta = total_runtime - s_last_total_runtime;

    for (int i = 0; i < PROCESS__MAX_RECORDS; ++i) {
        if (!s_processes[i].in_use || s_processes[i].handle == NULL) { continue; }
        for (UBaseType_t j = 0; j < count; ++j) {
            if (status_buf[j].xHandle == s_processes[i].handle) {
                uint32_t delta = status_buf[j].ulRunTimeCounter - s_processes[i].last_runtime_counter;
                s_processes[i].cpu_percent =
                    total_delta > 0 ? (uint32_t)(((uint64_t)delta * 100u) / total_delta) : 0u;
                s_processes[i].last_runtime_counter = status_buf[j].ulRunTimeCounter;
                s_processes[i].stack_high_water_bytes =
                    (uint32_t)(uxTaskGetStackHighWaterMark(s_processes[i].handle) * sizeof(StackType_t));
                break;
            }
        }
    }
    s_last_total_runtime = total_runtime;
    free(status_buf);
}

static void process__fill_snapshot_locked(const process__record_t *record, bruce_process_snapshot_t *out_snapshot) {
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->id = record->id;
    out_snapshot->state = record->state;
    strncpy(out_snapshot->name, record->name, BRUCE_PROCESS_NAME_MAX - 1);
    out_snapshot->stack_high_water_bytes = record->stack_high_water_bytes;
    out_snapshot->cpu_percent = record->cpu_percent;
    out_snapshot->memory_bytes = record->memory_bytes;
    out_snapshot->resource_count = record->resource_count;
    out_snapshot->built_in = record->built_in;
    out_snapshot->gui_requested = record->gui_requested;
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
 * absent. The slot remains unavailable until every attached waiter returns. */
static void process__teardown_locked(process__record_t *record, const bruce_process_status_t *status) {
    process__detach_wait_locked(record);
    for (int i = PROCESS__MAX_RESOURCES - 1; i >= 0; --i) {
        if (record->resources[i].active) {
            bruce_process_resource_cleanup_t cleanup = record->resources[i].cleanup;
            void *context = record->resources[i].context;
            record->resources[i].active = false;
            if (cleanup != NULL) { cleanup(context); }
        }
    }
    record->resource_count = 0;
    record->memory_bytes = 0;

    process__foreground_remove_locked(record->id);
    display__process_removed(record->id);
    process__foreground_recompute_locked();
    process__free_argv(record->argc, record->argv);
    record->argv = NULL;

    process__publish_completion_locked(record, status);
    record->in_use = false;
    record->handle = NULL;
    record->generation++;
    xEventGroupSetBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_EXITED);
}

static void process__trampoline(void *arg) {
    process__record_t *record = (process__record_t *)arg;
    FILE *stdio_input = NULL;
    FILE *stdio_output = NULL;
    FILE *stdio_error = NULL;

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
        process__teardown_locked(record, &status);
        process__unlock();
        vTaskDelete(NULL);
        return;
    }
    if (record->start_in_background) {
        record->state = BRUCE_PROCESS_BACKGROUND;
        display__process_state_changed(record->id, record->state);
    } else {
        record->state = BRUCE_PROCESS_BACKGROUND;
        process__foreground_push_locked(record->id);
    }
    process__unlock();

    /* Do not let a newly launched fullscreen GUI inherit the previous
     * foreground process's completed panel frame. Clear and present once before
     * application code starts; normal app redraw throttling can then remain
     * event-driven. */
    if (record->gui_requested && !record->start_in_background) process__clear_foreground_display();

    stdio__process_attach(record->stdio_session, &stdio_input, &stdio_output, &stdio_error);

    int exit_code = 0;
    if (record->process_entry != NULL) {
        record->process_entry(record->process_entry_context);
    } else if (record->entry != NULL) {
        exit_code = record->entry(record->argc, record->argv);
    }

    stdio__process_detach(stdio_input, stdio_output, stdio_error);

    process__lock();
    bruce_process_status_t status = {
        .reason = record->stop_requested ? BRUCE_PROCESS_TERMINATED : BRUCE_PROCESS_EXITED,
        .exit_code = record->stop_requested ? 0 : exit_code,
        .signal = record->stop_requested ? record->pending_signal : (bruce_process_signal_t)0,
    };
    process__teardown_locked(record, &status);
    process__unlock();

    vTaskDelete(NULL);
}

bruce_result_t process_registry__create(const process_create_params_t *params, bruce_process_id_t *out_process_id) {
    process__ensure_init();
    if (params == NULL || out_process_id == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool has_entry = params->entry != NULL;
    bool has_process_entry = params->process_entry != NULL;
    if (has_entry == has_process_entry) {
        /* exactly one of the two entry kinds must be set */
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_process_id = BRUCE_PROCESS_ID_INVALID;

    char **argv_copy = NULL;
    if (!process__dup_argv(params->argc, params->argv, &argv_copy)) { return BRUCE_ERR_NO_MEMORY; }

    process__lock();
    int slot = -1;
    for (int i = 0; i < PROCESS__MAX_RECORDS; ++i) {
        if (!s_processes[i].in_use && s_processes[i].waiter_count == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        process__unlock();
        process__free_argv(params->argc, argv_copy);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    process__record_t *record = &s_processes[slot];
    uint32_t generation = record->generation;
    memset(record, 0, sizeof(*record));
    record->generation = generation;
    record->in_use = true;
    record->id = process__allocate_id_locked();
    process__record_t *parent = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (parent != NULL) {
        record->stdio_session = parent->child_stdio_session;
        /* A routed shell must pass the same terminal session to commands it
         * launches, not only use it for the shell's own stdin/stdout. */
        record->child_stdio_session = parent->child_stdio_session;
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
    record->entry = params->entry;
    record->process_entry = params->process_entry;
    record->process_entry_context = params->process_entry_context;
    record->argc = params->argc > 0 ? params->argc : 0;
    record->argv = argv_copy;
    record->next_resource_id = 1;
    xEventGroupClearBits(
        s_process_events[slot],
        PROCESS__EVT_WAKE | PROCESS__EVT_EXITED | PROCESS__EVT_INPUT_WAKE | PROCESS__EVT_WAITER_WAKE
    );

    /* record->state is already BRUCE_PROCESS_STARTING from the memset above
     * (BRUCE_PROCESS_STARTING == 0); process__trampoline() performs the actual
     * foreground/background transition once the process begins running. */

    uint32_t stack_bytes = params->stack_bytes != 0 ? params->stack_bytes : PROCESS__DEFAULT_STACK_BYTES;
    BaseType_t created = xTaskCreate(
        process__trampoline, record->name, stack_bytes, record, tskIDLE_PRIORITY + 1, &record->handle
    );
    if (created != pdPASS) {
        process__free_argv(record->argc, record->argv);
        record->in_use = false;
        process__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    display__process_created(record->id, record->gui_requested);
    *out_process_id = record->id;
    process__unlock();
    return BRUCE_OK;
}

bruce_resource_id_t process_registry__resource_register(bruce_process_resource_cleanup_t cleanup, void *context) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_RESOURCE_ID_INVALID;
    }
    int free_slot = -1;
    for (int i = 0; i < PROCESS__MAX_RESOURCES; ++i) {
        if (!self->resources[i].active) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        process__unlock();
        return BRUCE_RESOURCE_ID_INVALID;
    }
    bruce_resource_id_t id = self->next_resource_id++;
    if (self->next_resource_id == BRUCE_RESOURCE_ID_INVALID) { self->next_resource_id = 1; }
    self->resources[free_slot].id = id;
    self->resources[free_slot].cleanup = cleanup;
    self->resources[free_slot].context = context;
    self->resources[free_slot].active = true;
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
    for (int i = 0; i < PROCESS__MAX_RESOURCES; ++i) {
        if (self->resources[i].active && self->resources[i].id == resource_id) {
            self->resources[i].context = context;
            process__unlock();
            return BRUCE_OK;
        }
    }
    process__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

void *
process_registry__resource_realloc(bruce_resource_id_t resource_id, void *context, size_t allocation_size) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return NULL;
    }
    for (int i = 0; i < PROCESS__MAX_RESOURCES; ++i) {
        if (self->resources[i].active && self->resources[i].id == resource_id &&
            self->resources[i].context == context) {
            void *resized = realloc(context, allocation_size);
            if (resized != NULL) self->resources[i].context = resized;
            process__unlock();
            return resized;
        }
    }
    process__unlock();
    return NULL;
}

bruce_result_t process_registry__resource_release(bruce_resource_id_t resource_id) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    for (int i = 0; i < PROCESS__MAX_RESOURCES; ++i) {
        if (self->resources[i].active && self->resources[i].id == resource_id) {
            self->resources[i].active = false;
            self->resource_count--;
            process__unlock();
            return BRUCE_OK;
        }
    }
    process__unlock();
    return BRUCE_ERR_NOT_FOUND;
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
    self->child_stdio_session = session;
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

bruce_result_t process_registry__input_wake_clear(bruce_process_id_t process_id) {
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    xEventGroupClearBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_INPUT_WAKE);
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process_registry__input_wake_wait(bruce_process_id_t process_id, uint32_t timeout_ms) {
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    EventGroupHandle_t events = s_process_events[process__slot_index_locked(record)];
    process__unlock();
    TickType_t ticks = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(events, PROCESS__EVT_INPUT_WAKE, pdTRUE, pdFALSE, ticks);
    return (bits & PROCESS__EVT_INPUT_WAKE) != 0 ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
}

void process_registry__input_wake(bruce_process_id_t process_id) {
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record != NULL) {
        xEventGroupSetBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_INPUT_WAKE);
    }
    process__unlock();
}

/* ---- Public core_sdk/process.h API ---- */

bruce_process_id_t process__current_id(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    bruce_process_id_t id = self != NULL ? self->id : BRUCE_PROCESS_ID_INVALID;
    process__unlock();
    return id;
}

bruce_process_signal_t process__current_signal(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    bruce_process_signal_t signal =
        self != NULL && self->stop_requested ? self->pending_signal : (bruce_process_signal_t)0;
    process__unlock();
    return signal;
}

bruce_result_t process__list(bruce_process_snapshot_t *snapshots, size_t capacity, size_t *out_count) {
    process__ensure_init();
    if (out_count == NULL || (capacity != 0 && snapshots == NULL)) { return BRUCE_ERR_INVALID_ARGUMENT; }
    process__lock();
    process__refresh_cpu_samples_locked();
    size_t written = 0;
    for (int i = 0; i < PROCESS__MAX_RECORDS && written < capacity; ++i) {
        if (s_processes[i].in_use) {
            process__fill_snapshot_locked(&s_processes[i], &snapshots[written]);
            written++;
        }
    }
    *out_count = written;
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__snapshot(bruce_process_id_t process_id, bruce_process_snapshot_t *out_snapshot) {
    process__ensure_init();
    if (out_snapshot == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    process__lock();
    process__refresh_cpu_samples_locked();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    process__fill_snapshot_locked(record, out_snapshot);
    process__unlock();
    return BRUCE_OK;
}

static bruce_result_t process__switch_relative(int direction) {
    process__ensure_init();
    process__lock();
    bruce_process_id_t anchor_id = s_effective_foreground;
    process__record_t *anchor = process__find_by_id_locked(anchor_id);
    if (anchor == NULL || !anchor->gui_requested) {
        anchor_id = BRUCE_PROCESS_ID_INVALID;
        for (int i = s_fg_depth - 1; i >= 0; --i) {
            process__record_t *stacked = process__find_by_id_locked(s_fg_stack[i]);
            if (stacked != NULL && stacked->gui_requested) {
                anchor_id = stacked->id;
                break;
            }
        }
    }

    int foreground_index = -1;
    for (int i = 0; i < PROCESS__MAX_RECORDS; ++i) {
        if (s_processes[i].in_use && s_processes[i].id == anchor_id) {
            foreground_index = i;
            break;
        }
    }
    for (int offset = 1; offset <= PROCESS__MAX_RECORDS; ++offset) {
        int index = foreground_index >= 0
                        ? (foreground_index + direction * offset + PROCESS__MAX_RECORDS) % PROCESS__MAX_RECORDS
                        : (direction > 0 ? offset - 1 : PROCESS__MAX_RECORDS - offset);
        process__record_t *candidate = &s_processes[index];
        if (candidate->in_use && candidate->gui_requested && candidate->state == BRUCE_PROCESS_BACKGROUND) {
            process__foreground_push_locked(candidate->id);
            process__unlock();
            return BRUCE_OK;
        }
    }
    process__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

bruce_result_t process_registry__switch_next(void) { return process__switch_relative(1); }

bruce_result_t process_registry__switch_previous(void) { return process__switch_relative(-1); }

bruce_process_id_t process_registry__foreground_id(void) {
    process__ensure_init();
    process__lock();
    bruce_process_id_t process_id = s_effective_foreground;
    process__unlock();
    return process_id;
}

bruce_result_t process__switch_next(void) {
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission_result != BRUCE_OK) return permission_result;
    return process_registry__switch_next();
}

bruce_result_t process__switch_previous(void) {
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission_result != BRUCE_OK) return permission_result;
    return process_registry__switch_previous();
}

bruce_result_t process__to_background(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (self->state != BRUCE_PROCESS_FOREGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    process__foreground_remove_locked(self->id);
    self->state = BRUCE_PROCESS_BACKGROUND;
    display__process_state_changed(self->id, self->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__to_foreground(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (self->state != BRUCE_PROCESS_BACKGROUND && self->state != BRUCE_PROCESS_FOREGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    if (!self->gui_requested) {
        self->gui_requested = true;
        display__process_set_gui_requested(self->id);
    }
    bool promoted = self->state == BRUCE_PROCESS_BACKGROUND;
    if (promoted) process__foreground_push_locked(self->id);
    process__unlock();
    if (promoted) process__clear_foreground_display();
    return BRUCE_OK;
}

bruce_result_t process__foreground(bruce_process_id_t process_id) {
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *target = process__find_by_id_locked(process_id);
    if (target == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (target->state == BRUCE_PROCESS_FOREGROUND) {
        process__unlock();
        return BRUCE_OK;
    }
    if (target->state != BRUCE_PROCESS_BACKGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    process__foreground_push_locked(process_id);
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__signal(bruce_process_id_t process_id, bruce_process_signal_t signal) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX ||
        (signal != BRUCE_PROCESS_SIGNAL_INT && signal != BRUCE_PROCESS_SIGNAL_KILL &&
         signal != BRUCE_PROCESS_SIGNAL_TERM)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (signal == BRUCE_PROCESS_SIGNAL_KILL) { return process__kill(process_id); }
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    record->stop_requested = true;
    record->pending_signal = signal;
    record->state = BRUCE_PROCESS_STOPPING;
    process__wake_locked(record);
    xEventGroupSetBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_INPUT_WAKE);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__terminate(bruce_process_id_t process_id) {
    return process__signal(process_id, BRUCE_PROCESS_SIGNAL_TERM);
}

bruce_result_t process__pause(bruce_process_id_t process_id) {
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (record->state == BRUCE_PROCESS_PAUSED) {
        process__unlock();
        return BRUCE_OK;
    }
    if (record->state != BRUCE_PROCESS_FOREGROUND && record->state != BRUCE_PROCESS_BACKGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    record->state_before_pause = record->state;
    record->state = BRUCE_PROCESS_PAUSED;
    record->pause_requested = true;
    process__wake_locked(record);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__resume(bruce_process_id_t process_id) {
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (record->state != BRUCE_PROCESS_PAUSED) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    record->pause_requested = false;
    record->state = BRUCE_PROCESS_BACKGROUND;
    process__wake_locked(record);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__kill(bruce_process_id_t process_id) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    TaskHandle_t handle = record->handle;
    bool is_self = handle != NULL && handle == xTaskGetCurrentTaskHandle();

    if (is_self) {
        /* Self-kill: tear down first, then delete; this call never returns. */
        bruce_process_status_t status = {
            .reason = BRUCE_PROCESS_KILLED,
            .exit_code = 0,
            .signal = BRUCE_PROCESS_SIGNAL_KILL,
        };
        process__teardown_locked(record, &status);
        process__unlock();
        vTaskDelete(NULL);
        return BRUCE_OK; /* unreachable */
    }

    /* Close Core service gates before deletion. The display hook waits past
     * any short raster critical section and transfers an in-flight frame to
     * worker ownership; foreground recomputation similarly revokes input. */
    record->stop_requested = true;
    record->pending_signal = BRUCE_PROCESS_SIGNAL_KILL;
    record->state = BRUCE_PROCESS_STOPPING;
    process__wake_locked(record);
    xEventGroupSetBits(s_process_events[process__slot_index_locked(record)], PROCESS__EVT_INPUT_WAKE);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();

    /* Arbitrary application-owned mutexes cannot be recovered after a force
     * delete, which remains the documented limitation of process__kill(). */
    if (handle != NULL) { vTaskDelete(handle); }
    bruce_process_status_t status = {
        .reason = BRUCE_PROCESS_KILLED,
        .exit_code = 0,
        .signal = BRUCE_PROCESS_SIGNAL_KILL,
    };
    process__teardown_locked(record, &status);
    process__unlock();
    return BRUCE_OK;
}

static bruce_result_t process__wait_common(
    bruce_process_id_t process_id, uint32_t timeout_ms, bruce_process_status_t *out_status
) {
    process__ensure_init();
    process__lock();
    process__completion_t *completion = process__find_completion_locked(process_id);
    if (completion != NULL) {
        if (out_status != NULL) {
            bruce_process_status_t status = completion->status;
            memset(completion, 0, sizeof(*completion));
            *out_status = status;
        }
        process__unlock();
        return BRUCE_OK;
    }
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    int slot = process__slot_index_locked(record);
    record->waiter_count++;
    if (out_status != NULL) record->status_waiter_count++;
    process__record_t *waiter = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (waiter != NULL) {
        waiter->wait_attached = true;
        waiter->wait_slot = slot;
        waiter->wait_for_status = out_status != NULL;
    }
    bool cancelled = waiter != NULL && waiter->stop_requested;
    EventGroupHandle_t events = s_process_events[slot];
    process__unlock();

    int64_t deadline_us = timeout_ms == UINT32_MAX ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    EventBits_t bits = 0;
    for (;;) {
        TickType_t ticks = portMAX_DELAY;
        if (timeout_ms != UINT32_MAX) {
            int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                ticks = 0;
            } else {
                uint64_t remaining_ms = ((uint64_t)remaining_us + 999u) / 1000u;
                ticks = pdMS_TO_TICKS(remaining_ms);
                if (ticks == 0) ticks = 1;
            }
        }
        if (!cancelled) {
            bits |= xEventGroupWaitBits(
                events, PROCESS__EVT_EXITED | PROCESS__EVT_WAITER_WAKE, pdTRUE, pdFALSE, ticks
            );
        }

        process__lock();
        completion = process__find_completion_locked(process_id);
        cancelled = waiter != NULL && waiter->stop_requested;
        bool absent = !s_processes[slot].in_use;
        bool timed_out = timeout_ms != UINT32_MAX && esp_timer_get_time() >= deadline_us;
        if (completion != NULL || cancelled || absent || timed_out) break;
        process__unlock();
    }

    bruce_result_t result = BRUCE_ERR_TIMEOUT;
    bruce_process_status_t status;
    if (out_status != NULL && completion != NULL) {
        status = completion->status;
        memset(completion, 0, sizeof(*completion));
        result = BRUCE_OK;
    } else if (out_status == NULL && (completion != NULL || (bits & PROCESS__EVT_EXITED) != 0)) {
        result = BRUCE_OK;
    } else if (cancelled) {
        result = BRUCE_ERR_CANCELLED;
    } else if (!s_processes[slot].in_use) {
        /* Another status waiter may have consumed the completion first. */
        result = BRUCE_ERR_NOT_FOUND;
    }

    if (waiter != NULL) {
        process__detach_wait_locked(waiter);
    } else {
        if (out_status != NULL && completion != NULL && completion->in_use && completion->waiter_pins > 0) {
            completion->waiter_pins--;
        }
        if (s_processes[slot].waiter_count > 0) s_processes[slot].waiter_count--;
        if (out_status != NULL && s_processes[slot].status_waiter_count > 0) {
            s_processes[slot].status_waiter_count--;
        }
    }
    process__unlock();

    if (result == BRUCE_OK && out_status != NULL) *out_status = status;
    return result;
}

bruce_result_t process__wait(bruce_process_id_t process_id, uint32_t timeout_ms) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (process_id == process__current_id()) { return BRUCE_ERR_INVALID_STATE; }
    return process__wait_common(process_id, timeout_ms, NULL);
}

bruce_result_t process__wait_status(
    bruce_process_id_t process_id, uint32_t timeout_ms, bruce_process_status_t *out_status
) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX || out_status == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (process_id == process__current_id()) { return BRUCE_ERR_INVALID_STATE; }
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission_result != BRUCE_OK) return permission_result;
    return process__wait_common(process_id, timeout_ms, out_status);
}

/* Shared implementation for runtime__sleep()/runtime__delay(): blocks while
 * paused (until resumed or stopped), returns BRUCE_ERR_CANCELLED as soon as a
 * stop is requested, and otherwise waits out `ms`.  When `interruptible` is
 * true and the process is background when the wait begins, being foregrounded
 * mid-wait also returns BRUCE_ERR_CANCELLED early. */
static bruce_result_t process__wait_ms(uint32_t ms, bool interruptible) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    process__unlock();
    if (self == NULL) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return BRUCE_OK;
    }
    int slot;
    process__lock();
    slot = process__slot_index_locked(self);
    bool was_background = self->state == BRUCE_PROCESS_BACKGROUND;
    process__unlock();

    int64_t deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
    for (;;) {
        process__lock();
        bool stopped = s_processes[slot].stop_requested;
        bool paused = s_processes[slot].pause_requested;
        bool now_foreground = s_processes[slot].state == BRUCE_PROCESS_FOREGROUND;
        process__unlock();

        if (stopped) { return BRUCE_ERR_CANCELLED; }
        if (paused) {
            xEventGroupWaitBits(s_process_events[slot], PROCESS__EVT_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        if (interruptible && was_background && now_foreground) { return BRUCE_ERR_CANCELLED; }

        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) { return BRUCE_OK; }
        uint64_t remaining_ms = ((uint64_t)remaining_us + 999u) / 1000u;
        TickType_t wait_ticks = pdMS_TO_TICKS(remaining_ms);
        if (wait_ticks == 0) wait_ticks = 1;
        (void)xEventGroupWaitBits(s_process_events[slot], PROCESS__EVT_WAKE, pdTRUE, pdFALSE, wait_ticks);
        /* Woken early; loop to re-check stop/pause/foreground state. */
    }
}

uint64_t runtime__now(void) { return (uint64_t)esp_timer_get_time() / 1000u; }

bruce_result_t runtime__sleep(uint32_t milliseconds) { return process__wait_ms(milliseconds, true); }

bruce_result_t runtime__delay(uint32_t milliseconds) { return process__wait_ms(milliseconds, false); }
