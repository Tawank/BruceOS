#include "task.h"

#include "core/display/display.h"
#include "core/input/input.h"
#include "core/stdio/stdio.h"
#include "core_sdk/display.h"
#include "core_sdk/permission.h"
#include "core_sdk/task.h"

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TASK__MAX_RECORDS 8
#define TASK__MAX_RESOURCES 8
#define TASK__FOREGROUND_STACK_MAX TASK__MAX_RECORDS
#define TASK__DEFAULT_STACK_BYTES 4096u
#define TASK__EVT_WAKE (1u << 0)
#define TASK__EVT_EXITED (1u << 1)
#define TASK__EVT_INPUT_WAKE (1u << 2)

typedef struct {
    bruce_resource_id_t id;
    bruce_task_resource_cleanup_t cleanup;
    void *context;
    bool active;
} task__resource_slot_t;

typedef struct {
    bool in_use;
    uint32_t generation;
    bruce_task_id_t id;
    char name[BRUCE_TASK_NAME_MAX];
    bruce_task_state_t state;
    bruce_task_state_t state_before_pause;
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

    void (*task_entry)(void *context);
    void *task_entry_context;
    volatile bool stop_requested;
    volatile bool pause_requested;

    task__resource_slot_t resources[TASK__MAX_RESOURCES];
    bruce_resource_id_t next_resource_id;
    size_t resource_count;
    size_t memory_bytes;

    uint32_t last_runtime_counter;
    uint32_t cpu_percent;
    uint32_t stack_high_water_bytes;
} task__record_t;

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static task__record_t s_tasks[TASK__MAX_RECORDS];
static EventGroupHandle_t s_task_events[TASK__MAX_RECORDS];
static bruce_task_id_t s_next_task_id = 1;

static bruce_task_id_t s_fg_stack[TASK__FOREGROUND_STACK_MAX];
static int s_fg_depth;
static bruce_task_id_t s_effective_foreground;

static uint32_t s_last_total_runtime;

static void task__ensure_init(void) {
    if (s_lock != NULL) { return; }
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
        for (int i = 0; i < TASK__MAX_RECORDS; ++i) { s_task_events[i] = xEventGroupCreate(); }
    }
    portEXIT_CRITICAL(&s_init_mux);
}

static void task__lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }

static void task__unlock(void) { xSemaphoreGiveRecursive(s_lock); }

/* Caller must hold the lock. */
static task__record_t *task__find_by_id_locked(bruce_task_id_t id) {
    if (id == BRUCE_TASK_ID_INVALID) { return NULL; }
    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (s_tasks[i].in_use && s_tasks[i].id == id) { return &s_tasks[i]; }
    }
    return NULL;
}

/* Caller must hold the lock. */
static task__record_t *task__find_by_handle_locked(TaskHandle_t handle) {
    if (handle == NULL) { return NULL; }
    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (s_tasks[i].in_use && s_tasks[i].handle == handle) { return &s_tasks[i]; }
    }
    return NULL;
}

static int task__slot_index_locked(const task__record_t *record) { return (int)(record - s_tasks); }

static void task__wake_locked(task__record_t *record) {
    xEventGroupSetBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_WAKE);
}

/* Removes `id` from the foreground stack if (and only if) it is currently on
 * top, restoring the task beneath it (if any) to BRUCE_TASK_FOREGROUND.
 * Caller must hold the lock. */
static void task__foreground_notify_locked(bruce_task_id_t previous, bruce_task_id_t current) {
    input__foreground_changed(current);
    if (previous != BRUCE_TASK_ID_INVALID) {
        task__record_t *record = task__find_by_id_locked(previous);
        if (record != NULL) {
            xEventGroupSetBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_INPUT_WAKE);
        }
    }
    if (current != BRUCE_TASK_ID_INVALID) {
        task__record_t *record = task__find_by_id_locked(current);
        if (record != NULL) {
            xEventGroupSetBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_INPUT_WAKE);
        }
    }
}

/* Compact the stack and derive all runnable foreground/background states from
 * it. Paused tasks retain their position but are temporarily ineligible. */
static void task__foreground_recompute_locked(void) {
    int write = 0;
    bruce_task_id_t next = BRUCE_TASK_ID_INVALID;
    for (int i = 0; i < s_fg_depth; ++i) {
        task__record_t *record = task__find_by_id_locked(s_fg_stack[i]);
        if (record == NULL || record->state == BRUCE_TASK_STOPPING) { continue; }
        s_fg_stack[write++] = record->id;
        if (record->state != BRUCE_TASK_PAUSED && record->state != BRUCE_TASK_STARTING) { next = record->id; }
    }
    s_fg_depth = write;

    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        task__record_t *record = &s_tasks[i];
        if (!record->in_use || record->state == BRUCE_TASK_STARTING || record->state == BRUCE_TASK_PAUSED ||
            record->state == BRUCE_TASK_STOPPING) {
            continue;
        }
        bruce_task_state_t new_state = record->id == next ? BRUCE_TASK_FOREGROUND : BRUCE_TASK_BACKGROUND;
        if (record->state != new_state) {
            record->state = new_state;
            task__wake_locked(record);
            display__task_state_changed(record->id, new_state);
        }
    }

    if (next != s_effective_foreground) {
        bruce_task_id_t previous = s_effective_foreground;
        s_effective_foreground = next;
        task__foreground_notify_locked(previous, next);
    }
}

static void task__foreground_remove_locked(bruce_task_id_t id) {
    int write = 0;
    for (int i = 0; i < s_fg_depth; ++i) {
        if (s_fg_stack[i] != id) { s_fg_stack[write++] = s_fg_stack[i]; }
    }
    s_fg_depth = write;
}

static void task__foreground_push_locked(bruce_task_id_t id) {
    task__foreground_remove_locked(id);
    if (s_fg_depth < TASK__FOREGROUND_STACK_MAX) { s_fg_stack[s_fg_depth++] = id; }
    task__foreground_recompute_locked();
}

static void task__refresh_cpu_samples_locked(void) {
    const size_t status_capacity = TASK__MAX_RECORDS + 8u;
    TaskStatus_t *status_buf = malloc(status_capacity * sizeof(*status_buf));
    if (status_buf == NULL) return;
    uint32_t total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(status_buf, status_capacity, &total_runtime);
    uint32_t total_delta = total_runtime - s_last_total_runtime;

    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (!s_tasks[i].in_use || s_tasks[i].handle == NULL) { continue; }
        for (UBaseType_t j = 0; j < count; ++j) {
            if (status_buf[j].xHandle == s_tasks[i].handle) {
                uint32_t delta = status_buf[j].ulRunTimeCounter - s_tasks[i].last_runtime_counter;
                s_tasks[i].cpu_percent =
                    total_delta > 0 ? (uint32_t)(((uint64_t)delta * 100u) / total_delta) : 0u;
                s_tasks[i].last_runtime_counter = status_buf[j].ulRunTimeCounter;
                s_tasks[i].stack_high_water_bytes =
                    (uint32_t)(uxTaskGetStackHighWaterMark(s_tasks[i].handle) * sizeof(StackType_t));
                break;
            }
        }
    }
    s_last_total_runtime = total_runtime;
    free(status_buf);
}

static void task__fill_snapshot_locked(const task__record_t *record, bruce_task_snapshot_t *out_snapshot) {
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->id = record->id;
    out_snapshot->state = record->state;
    strncpy(out_snapshot->name, record->name, BRUCE_TASK_NAME_MAX - 1);
    out_snapshot->stack_high_water_bytes = record->stack_high_water_bytes;
    out_snapshot->cpu_percent = record->cpu_percent;
    out_snapshot->memory_bytes = record->memory_bytes;
    out_snapshot->resource_count = record->resource_count;
    out_snapshot->built_in = record->built_in;
    out_snapshot->gui_requested = record->gui_requested;
}

static void task__free_argv(int argc, char **argv) {
    if (argv == NULL) { return; }
    for (int i = 0; i < argc; ++i) { free(argv[i]); }
    free(argv);
}

static bool task__dup_argv(int argc, char *const *src_argv, char ***out_argv) {
    *out_argv = NULL;
    if (argc <= 0) { return true; }
    char **copy = calloc((size_t)argc + 1u, sizeof(char *));
    if (copy == NULL) { return false; }
    for (int i = 0; i < argc; ++i) {
        const char *source = (src_argv != NULL && src_argv[i] != NULL) ? src_argv[i] : "";
        size_t length = strlen(source) + 1;
        copy[i] = malloc(length);
        if (copy[i] == NULL) {
            task__free_argv(i, copy);
            return false;
        }
        memcpy(copy[i], source, length);
    }
    *out_argv = copy;
    return true;
}

/* Runs every active resource's cleanup callback in reverse-registration
 * order, pops the task off the foreground stack if it was on top, and marks
 * the slot as free for reuse.  Caller must hold the lock; called for both
 * normal exit and task__kill(). */
static void task__teardown_locked(task__record_t *record) {
    for (int i = TASK__MAX_RESOURCES - 1; i >= 0; --i) {
        if (record->resources[i].active) {
            bruce_task_resource_cleanup_t cleanup = record->resources[i].cleanup;
            void *context = record->resources[i].context;
            record->resources[i].active = false;
            if (cleanup != NULL) { cleanup(context); }
        }
    }
    record->resource_count = 0;
    record->memory_bytes = 0;

    task__foreground_remove_locked(record->id);
    display__task_removed(record->id);
    task__foreground_recompute_locked();
    task__free_argv(record->argc, record->argv);
    record->argv = NULL;

    xEventGroupSetBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_EXITED);
    record->in_use = false;
    record->handle = NULL;
    record->generation++;
}

static void task__trampoline(void *arg) {
    task__record_t *record = (task__record_t *)arg;
    FILE *stdio_input = NULL;
    FILE *stdio_output = NULL;
    FILE *stdio_error = NULL;

    /* The record was created in BRUCE_TASK_STARTING; this is the first thing
     * the new task does once FreeRTOS actually schedules it, and still runs
     * before record->entry() sees a single instruction. */
    task__lock();
    if (record->start_in_background) {
        record->state = BRUCE_TASK_BACKGROUND;
        display__task_state_changed(record->id, record->state);
    } else {
        record->state = BRUCE_TASK_BACKGROUND;
        task__foreground_push_locked(record->id);
    }
    task__unlock();

    /* Do not let a newly launched fullscreen GUI inherit the previous
     * foreground task's completed panel frame. Clear and present once before
     * application code starts; normal app redraw throttling can then remain
     * event-driven. */
    if (record->gui_requested && !record->start_in_background) {
        if (display__begin_frame() == BRUCE_OK) {
            (void)display__fill_screen(BRUCE_COLOR_BLACK);
            (void)display__present();
        }
    }

    stdio__task_attach(record->stdio_session, &stdio_input, &stdio_output, &stdio_error);

    if (record->task_entry != NULL) {
        record->task_entry(record->task_entry_context);
    } else if (record->entry != NULL) {
        (void)record->entry(record->argc, record->argv);
    }

    stdio__task_detach(stdio_input, stdio_output, stdio_error);

    task__lock();
    task__teardown_locked(record);
    task__unlock();

    vTaskDelete(NULL);
}

bruce_result_t task_registry__create(const task_create_params_t *params, bruce_task_id_t *out_task_id) {
    task__ensure_init();
    if (params == NULL || out_task_id == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool has_entry = params->entry != NULL;
    bool has_task_entry = params->task_entry != NULL;
    if (has_entry == has_task_entry) {
        /* exactly one of the two entry kinds must be set */
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_task_id = BRUCE_TASK_ID_INVALID;

    char **argv_copy = NULL;
    if (!task__dup_argv(params->argc, params->argv, &argv_copy)) { return BRUCE_ERR_NO_MEMORY; }

    task__lock();
    int slot = -1;
    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (!s_tasks[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        task__unlock();
        task__free_argv(params->argc, argv_copy);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    task__record_t *record = &s_tasks[slot];
    uint32_t generation = record->generation;
    memset(record, 0, sizeof(*record));
    record->generation = generation;
    record->in_use = true;
    record->id = s_next_task_id++;
    task__record_t *parent = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (parent != NULL) { record->stdio_session = parent->child_stdio_session; }
    if (s_next_task_id == BRUCE_TASK_ID_INVALID) { s_next_task_id = 1; /* skip 0 on wraparound */ }
    strncpy(
        record->name,
        params->name != NULL && params->name[0] != '\0' ? params->name : "app",
        BRUCE_TASK_NAME_MAX - 1
    );
    record->built_in = params->built_in;
    record->gui_requested = params->gui_requested;
    if (params->permission_key != NULL && params->permission_key[0] != '\0') {
        strncpy(record->permission_key, params->permission_key, BRUCE_PERMISSION_FILE_NAME_MAX - 1);
        record->permission_key[BRUCE_PERMISSION_FILE_NAME_MAX - 1] = '\0';
    }
    record->start_in_background = params->start_in_background;
    record->entry = params->entry;
    record->task_entry = params->task_entry;
    record->task_entry_context = params->task_entry_context;
    record->argc = params->argc > 0 ? params->argc : 0;
    record->argv = argv_copy;
    record->next_resource_id = 1;
    xEventGroupClearBits(s_task_events[slot], TASK__EVT_WAKE | TASK__EVT_EXITED | TASK__EVT_INPUT_WAKE);

    /* record->state is already BRUCE_TASK_STARTING from the memset above
     * (BRUCE_TASK_STARTING == 0); task__trampoline() performs the actual
     * foreground/background transition once the task begins running. */

    uint32_t stack_bytes = params->stack_bytes != 0 ? params->stack_bytes : TASK__DEFAULT_STACK_BYTES;
    BaseType_t created = xTaskCreate(
        task__trampoline, record->name, stack_bytes, record, tskIDLE_PRIORITY + 1, &record->handle
    );
    if (created != pdPASS) {
        task__free_argv(record->argc, record->argv);
        record->in_use = false;
        task__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    display__task_created(record->id, record->gui_requested);
    *out_task_id = record->id;
    task__unlock();
    return BRUCE_OK;
}

bruce_resource_id_t task_registry__resource_register(bruce_task_resource_cleanup_t cleanup, void *context) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        task__unlock();
        return BRUCE_RESOURCE_ID_INVALID;
    }
    int free_slot = -1;
    for (int i = 0; i < TASK__MAX_RESOURCES; ++i) {
        if (!self->resources[i].active) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        task__unlock();
        return BRUCE_RESOURCE_ID_INVALID;
    }
    bruce_resource_id_t id = self->next_resource_id++;
    if (self->next_resource_id == BRUCE_RESOURCE_ID_INVALID) { self->next_resource_id = 1; }
    self->resources[free_slot].id = id;
    self->resources[free_slot].cleanup = cleanup;
    self->resources[free_slot].context = context;
    self->resources[free_slot].active = true;
    self->resource_count++;
    task__unlock();
    return id;
}

bruce_result_t task_registry__resource_update(bruce_resource_id_t resource_id, void *context) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    for (int i = 0; i < TASK__MAX_RESOURCES; ++i) {
        if (self->resources[i].active && self->resources[i].id == resource_id) {
            self->resources[i].context = context;
            task__unlock();
            return BRUCE_OK;
        }
    }
    task__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

void *task_registry__resource_realloc(
    bruce_resource_id_t resource_id, void *context, size_t allocation_size
) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        task__unlock();
        return NULL;
    }
    for (int i = 0; i < TASK__MAX_RESOURCES; ++i) {
        if (self->resources[i].active && self->resources[i].id == resource_id &&
            self->resources[i].context == context) {
            void *resized = realloc(context, allocation_size);
            if (resized != NULL) self->resources[i].context = resized;
            task__unlock();
            return resized;
        }
    }
    task__unlock();
    return NULL;
}

bruce_result_t task_registry__resource_release(bruce_resource_id_t resource_id) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    for (int i = 0; i < TASK__MAX_RESOURCES; ++i) {
        if (self->resources[i].active && self->resources[i].id == resource_id) {
            self->resources[i].active = false;
            self->resource_count--;
            task__unlock();
            return BRUCE_OK;
        }
    }
    task__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

void task_registry__account_memory(int64_t delta_bytes) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self != NULL) {
        if (delta_bytes >= 0 || (size_t)(-delta_bytes) <= self->memory_bytes) {
            self->memory_bytes = (size_t)((int64_t)self->memory_bytes + delta_bytes);
        } else {
            self->memory_bytes = 0;
        }
    }
    task__unlock();
}

bruce_result_t task_registry__current_context(
    bool *out_built_in, char *out_permission_key, size_t permission_key_size, bool *out_gui_requested
) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (out_built_in != NULL) { *out_built_in = self->built_in; }
    if (out_permission_key != NULL && permission_key_size > 0) {
        strncpy(out_permission_key, self->permission_key, permission_key_size - 1);
        out_permission_key[permission_key_size - 1] = '\0';
    }
    if (out_gui_requested != NULL) { *out_gui_requested = self->gui_requested; }
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task_registry__set_child_stdio_session(uint32_t session) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    self->child_stdio_session = session;
    task__unlock();
    return BRUCE_OK;
}

uint32_t task_registry__current_stdio_session(void) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    uint32_t session = self != NULL ? self->stdio_session : BRUCE_STDIO_SESSION_INVALID;
    task__unlock();
    return session;
}

bruce_result_t task_registry__input_wake_clear(bruce_task_id_t task_id) {
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    xEventGroupClearBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_INPUT_WAKE);
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task_registry__input_wake_wait(bruce_task_id_t task_id, uint32_t timeout_ms) {
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    EventGroupHandle_t events = s_task_events[task__slot_index_locked(record)];
    task__unlock();
    TickType_t ticks = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(events, TASK__EVT_INPUT_WAKE, pdTRUE, pdFALSE, ticks);
    return (bits & TASK__EVT_INPUT_WAKE) != 0 ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
}

void task_registry__input_wake(bruce_task_id_t task_id) {
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record != NULL) {
        xEventGroupSetBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_INPUT_WAKE);
    }
    task__unlock();
}

/* ---- Public core_sdk/task.h API ---- */

bruce_task_id_t task__current_id(void) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    bruce_task_id_t id = self != NULL ? self->id : BRUCE_TASK_ID_INVALID;
    task__unlock();
    return id;
}

bruce_result_t task__list(bruce_task_snapshot_t *snapshots, size_t capacity, size_t *out_count) {
    task__ensure_init();
    if (out_count == NULL || (capacity != 0 && snapshots == NULL)) { return BRUCE_ERR_INVALID_ARGUMENT; }
    task__lock();
    task__refresh_cpu_samples_locked();
    size_t written = 0;
    for (int i = 0; i < TASK__MAX_RECORDS && written < capacity; ++i) {
        if (s_tasks[i].in_use) {
            task__fill_snapshot_locked(&s_tasks[i], &snapshots[written]);
            written++;
        }
    }
    *out_count = written;
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__snapshot(bruce_task_id_t task_id, bruce_task_snapshot_t *out_snapshot) {
    task__ensure_init();
    if (out_snapshot == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    task__lock();
    task__refresh_cpu_samples_locked();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    task__fill_snapshot_locked(record, out_snapshot);
    task__unlock();
    return BRUCE_OK;
}

static bruce_result_t task__switch_relative(int direction) {
    task__ensure_init();
    task__lock();
    bruce_task_id_t anchor_id = s_effective_foreground;
    task__record_t *anchor = task__find_by_id_locked(anchor_id);
    if (anchor == NULL || !anchor->gui_requested) {
        anchor_id = BRUCE_TASK_ID_INVALID;
        for (int i = s_fg_depth - 1; i >= 0; --i) {
            task__record_t *stacked = task__find_by_id_locked(s_fg_stack[i]);
            if (stacked != NULL && stacked->gui_requested) {
                anchor_id = stacked->id;
                break;
            }
        }
    }

    int foreground_index = -1;
    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (s_tasks[i].in_use && s_tasks[i].id == anchor_id) {
            foreground_index = i;
            break;
        }
    }
    for (int offset = 1; offset <= TASK__MAX_RECORDS; ++offset) {
        int index = foreground_index >= 0
                        ? (foreground_index + direction * offset + TASK__MAX_RECORDS) % TASK__MAX_RECORDS
                        : (direction > 0 ? offset - 1 : TASK__MAX_RECORDS - offset);
        task__record_t *candidate = &s_tasks[index];
        if (candidate->in_use && candidate->gui_requested && candidate->state == BRUCE_TASK_BACKGROUND) {
            task__foreground_push_locked(candidate->id);
            task__unlock();
            return BRUCE_OK;
        }
    }
    task__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

bruce_result_t task_registry__switch_next(void) { return task__switch_relative(1); }

bruce_result_t task_registry__switch_previous(void) { return task__switch_relative(-1); }

bruce_result_t task__switch_next(void) {
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_TASK);
    if (permission_result != BRUCE_OK) return permission_result;
    return task_registry__switch_next();
}

bruce_result_t task__switch_previous(void) {
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_TASK);
    if (permission_result != BRUCE_OK) return permission_result;
    return task_registry__switch_previous();
}

bruce_result_t task__to_background(void) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (self->state != BRUCE_TASK_FOREGROUND) {
        task__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    task__foreground_remove_locked(self->id);
    self->state = BRUCE_TASK_BACKGROUND;
    display__task_state_changed(self->id, self->state);
    task__foreground_recompute_locked();
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__foreground(bruce_task_id_t task_id) {
    if (task_id != task__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_TASK);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    task__ensure_init();
    task__lock();
    task__record_t *target = task__find_by_id_locked(task_id);
    if (target == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (target->state == BRUCE_TASK_FOREGROUND) {
        task__unlock();
        return BRUCE_OK;
    }
    if (target->state != BRUCE_TASK_BACKGROUND) {
        task__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    task__foreground_push_locked(task_id);
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__stop(bruce_task_id_t task_id) {
    if (task_id != task__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_TASK);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    record->stop_requested = true;
    record->state = BRUCE_TASK_STOPPING;
    task__wake_locked(record);
    display__task_state_changed(record->id, record->state);
    task__foreground_recompute_locked();
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__pause(bruce_task_id_t task_id) {
    if (task_id != task__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_TASK);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (record->state == BRUCE_TASK_PAUSED) {
        task__unlock();
        return BRUCE_OK;
    }
    if (record->state != BRUCE_TASK_FOREGROUND && record->state != BRUCE_TASK_BACKGROUND) {
        task__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    record->state_before_pause = record->state;
    record->state = BRUCE_TASK_PAUSED;
    record->pause_requested = true;
    task__wake_locked(record);
    display__task_state_changed(record->id, record->state);
    task__foreground_recompute_locked();
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__resume(bruce_task_id_t task_id) {
    if (task_id != task__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_TASK);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (record->state != BRUCE_TASK_PAUSED) {
        task__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    record->pause_requested = false;
    record->state = BRUCE_TASK_BACKGROUND;
    task__wake_locked(record);
    display__task_state_changed(record->id, record->state);
    task__foreground_recompute_locked();
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__kill(bruce_task_id_t task_id) {
    if (task_id != task__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_TASK);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    TaskHandle_t handle = record->handle;
    bool is_self = handle != NULL && handle == xTaskGetCurrentTaskHandle();

    if (is_self) {
        /* Self-kill: tear down first, then delete; this call never returns. */
        task__teardown_locked(record);
        task__unlock();
        vTaskDelete(NULL);
        return BRUCE_OK; /* unreachable */
    }

    /* Close Core service gates before deletion. The display hook waits past
     * any short raster critical section and transfers an in-flight frame to
     * worker ownership; foreground recomputation similarly revokes input. */
    record->stop_requested = true;
    record->state = BRUCE_TASK_STOPPING;
    task__wake_locked(record);
    display__task_state_changed(record->id, record->state);
    task__foreground_recompute_locked();

    /* Arbitrary application-owned mutexes cannot be recovered after a force
     * delete, which remains the documented limitation of task__kill(). */
    if (handle != NULL) { vTaskDelete(handle); }
    task__teardown_locked(record);
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__wait(bruce_task_id_t task_id, uint32_t timeout_ms) {
    task__ensure_init();
    task__lock();
    task__record_t *record = task__find_by_id_locked(task_id);
    if (record == NULL) {
        task__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    int slot = task__slot_index_locked(record);
    uint32_t generation = record->generation;
    EventGroupHandle_t events = s_task_events[slot];
    task__unlock();

    EventBits_t bits =
        xEventGroupWaitBits(events, TASK__EVT_EXITED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    task__lock();
    bool recycled = s_tasks[slot].generation != generation;
    task__unlock();

    if (recycled || (bits & TASK__EVT_EXITED) != 0) { return BRUCE_OK; }
    return BRUCE_ERR_TIMEOUT;
}

/* Shared implementation for runtime__sleep()/runtime__delay(): blocks while
 * paused (until resumed or stopped), returns BRUCE_ERR_CANCELLED as soon as a
 * stop is requested, and otherwise waits out `ms`.  When `interruptible` is
 * true and the task is background when the wait begins, being foregrounded
 * mid-wait also returns BRUCE_ERR_CANCELLED early. */
static bruce_result_t task__wait_ms(uint32_t ms, bool interruptible) {
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    task__unlock();
    if (self == NULL) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return BRUCE_OK;
    }
    int slot;
    task__lock();
    slot = task__slot_index_locked(self);
    bool was_background = self->state == BRUCE_TASK_BACKGROUND;
    task__unlock();

    int64_t deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
    for (;;) {
        task__lock();
        bool stopped = s_tasks[slot].stop_requested;
        bool paused = s_tasks[slot].pause_requested;
        bool now_foreground = s_tasks[slot].state == BRUCE_TASK_FOREGROUND;
        task__unlock();

        if (stopped) { return BRUCE_ERR_CANCELLED; }
        if (paused) {
            xEventGroupWaitBits(s_task_events[slot], TASK__EVT_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        if (interruptible && was_background && now_foreground) { return BRUCE_ERR_CANCELLED; }

        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) { return BRUCE_OK; }
        uint64_t remaining_ms = ((uint64_t)remaining_us + 999u) / 1000u;
        TickType_t wait_ticks = pdMS_TO_TICKS(remaining_ms);
        if (wait_ticks == 0) wait_ticks = 1;
        (void)xEventGroupWaitBits(
            s_task_events[slot], TASK__EVT_WAKE, pdTRUE, pdFALSE, wait_ticks
        );
        /* Woken early; loop to re-check stop/pause/foreground state. */
    }
}

uint64_t runtime__now(void) { return (uint64_t)esp_timer_get_time() / 1000u; }

bruce_result_t runtime__sleep(uint32_t milliseconds) { return task__wait_ms(milliseconds, true); }

bruce_result_t runtime__delay(uint32_t milliseconds) { return task__wait_ms(milliseconds, false); }
