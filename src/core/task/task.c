#include "task.h"

#include "core_sdk/task.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TASK__MAX_RECORDS 16
#define TASK__MAX_RESOURCES 16
#define TASK__FOREGROUND_STACK_MAX 8
#define TASK__DEFAULT_STACK_BYTES 4096u
#define TASK__EVT_WAKE (1u << 0)
#define TASK__EVT_EXITED (1u << 1)

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
    TaskHandle_t handle;

    bruce_app_entry_t entry;
    int argc;
    char **argv;

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

static uint32_t s_last_total_runtime;

static void task__ensure_init(void)
{
    if (s_lock != NULL) {
        return;
    }
    portENTER_CRITICAL(&s_init_mux);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
        for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
            s_task_events[i] = xEventGroupCreate();
        }
    }
    portEXIT_CRITICAL(&s_init_mux);
}

static void task__lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void task__unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

/* Caller must hold the lock. */
static task__record_t *task__find_by_id_locked(bruce_task_id_t id)
{
    if (id == BRUCE_TASK_ID_INVALID) {
        return NULL;
    }
    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (s_tasks[i].in_use && s_tasks[i].id == id) {
            return &s_tasks[i];
        }
    }
    return NULL;
}

/* Caller must hold the lock. */
static task__record_t *task__find_by_handle_locked(TaskHandle_t handle)
{
    if (handle == NULL) {
        return NULL;
    }
    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (s_tasks[i].in_use && s_tasks[i].handle == handle) {
            return &s_tasks[i];
        }
    }
    return NULL;
}

static int task__slot_index_locked(const task__record_t *record)
{
    return (int)(record - s_tasks);
}

static void task__wake_locked(task__record_t *record)
{
    xEventGroupSetBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_WAKE);
}

/* Removes `id` from the foreground stack if (and only if) it is currently on
 * top, restoring the task beneath it (if any) to BRUCE_TASK_FOREGROUND.
 * Caller must hold the lock. */
static void task__foreground_stack_pop_if_top_locked(bruce_task_id_t id)
{
    if (s_fg_depth <= 0 || s_fg_stack[s_fg_depth - 1] != id) {
        return;
    }
    s_fg_depth--;
    if (s_fg_depth > 0) {
        task__record_t *restored = task__find_by_id_locked(s_fg_stack[s_fg_depth - 1]);
        if (restored != NULL) {
            restored->state = BRUCE_TASK_FOREGROUND;
            task__wake_locked(restored);
        }
    }
}

/* Pushes `id` on top of the foreground stack, backgrounding the previous top
 * (if any and if different).  Caller must hold the lock. */
static void task__foreground_stack_push_locked(bruce_task_id_t id, task__record_t *record)
{
    if (s_fg_depth > 0 && s_fg_stack[s_fg_depth - 1] != id) {
        task__record_t *previous_top = task__find_by_id_locked(s_fg_stack[s_fg_depth - 1]);
        if (previous_top != NULL) {
            previous_top->state = BRUCE_TASK_BACKGROUND;
            task__wake_locked(previous_top);
        }
    }
    if (s_fg_depth == 0 || s_fg_stack[s_fg_depth - 1] != id) {
        if (s_fg_depth < TASK__FOREGROUND_STACK_MAX) {
            s_fg_stack[s_fg_depth++] = id;
        }
    }
    record->state = BRUCE_TASK_FOREGROUND;
    task__wake_locked(record);
}

static void task__refresh_cpu_samples_locked(void)
{
    static TaskStatus_t status_buf[TASK__MAX_RECORDS + 8];
    uint32_t total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(status_buf, sizeof(status_buf) / sizeof(status_buf[0]), &total_runtime);
    uint32_t total_delta = total_runtime - s_last_total_runtime;

    for (int i = 0; i < TASK__MAX_RECORDS; ++i) {
        if (!s_tasks[i].in_use || s_tasks[i].handle == NULL) {
            continue;
        }
        for (UBaseType_t j = 0; j < count; ++j) {
            if (status_buf[j].xHandle == s_tasks[i].handle) {
                uint32_t delta = status_buf[j].ulRunTimeCounter - s_tasks[i].last_runtime_counter;
                s_tasks[i].cpu_percent = total_delta > 0 ? (uint32_t)(((uint64_t)delta * 100u) / total_delta) : 0u;
                s_tasks[i].last_runtime_counter = status_buf[j].ulRunTimeCounter;
                s_tasks[i].stack_high_water_bytes =
                    (uint32_t)(uxTaskGetStackHighWaterMark(s_tasks[i].handle) * sizeof(StackType_t));
                break;
            }
        }
    }
    s_last_total_runtime = total_runtime;
}

static void task__fill_snapshot_locked(const task__record_t *record, bruce_task_snapshot_t *out_snapshot)
{
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

static void task__free_argv(int argc, char **argv)
{
    if (argv == NULL) {
        return;
    }
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static bool task__dup_argv(int argc, char *const *src_argv, char ***out_argv)
{
    *out_argv = NULL;
    if (argc <= 0) {
        return true;
    }
    char **copy = calloc((size_t)argc, sizeof(char *));
    if (copy == NULL) {
        return false;
    }
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
static void task__teardown_locked(task__record_t *record)
{
    for (int i = TASK__MAX_RESOURCES - 1; i >= 0; --i) {
        if (record->resources[i].active) {
            bruce_task_resource_cleanup_t cleanup = record->resources[i].cleanup;
            void *context = record->resources[i].context;
            record->resources[i].active = false;
            if (cleanup != NULL) {
                cleanup(context);
            }
        }
    }
    record->resource_count = 0;
    record->memory_bytes = 0;

    task__foreground_stack_pop_if_top_locked(record->id);
    task__free_argv(record->argc, record->argv);
    record->argv = NULL;

    xEventGroupSetBits(s_task_events[task__slot_index_locked(record)], TASK__EVT_EXITED);
    record->in_use = false;
    record->handle = NULL;
    record->generation++;
}

static void task__trampoline(void *arg)
{
    task__record_t *record = (task__record_t *)arg;
    int result = record->entry != NULL ? record->entry(record->argc, record->argv) : -1;
    (void)result;

    task__lock();
    task__teardown_locked(record);
    task__unlock();

    vTaskDelete(NULL);
}

bruce_result_t task_registry__create(const task_create_params_t *params, bruce_task_id_t *out_task_id)
{
    task__ensure_init();
    if (params == NULL || params->entry == NULL || out_task_id == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_task_id = BRUCE_TASK_ID_INVALID;

    char **argv_copy = NULL;
    if (!task__dup_argv(params->argc, params->argv, &argv_copy)) {
        return BRUCE_ERR_NO_MEMORY;
    }

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
    memset(record, 0, sizeof(*record));
    record->in_use = true;
    record->id = s_next_task_id++;
    if (s_next_task_id == BRUCE_TASK_ID_INVALID) {
        s_next_task_id = 1; /* skip 0 on wraparound */
    }
    strncpy(record->name, params->name != NULL && params->name[0] != '\0' ? params->name : "app",
            BRUCE_TASK_NAME_MAX - 1);
    record->built_in = params->built_in;
    record->gui_requested = params->gui_requested;
    record->entry = params->entry;
    record->argc = params->argc > 0 ? params->argc : 0;
    record->argv = argv_copy;
    record->next_resource_id = 1;
    xEventGroupClearBits(s_task_events[slot], TASK__EVT_WAKE | TASK__EVT_EXITED);

    if (params->start_in_background) {
        record->state = BRUCE_TASK_BACKGROUND;
    } else {
        task__foreground_stack_push_locked(record->id, record);
    }

    uint32_t stack_bytes = params->stack_bytes != 0 ? params->stack_bytes : TASK__DEFAULT_STACK_BYTES;
    BaseType_t created = xTaskCreate(task__trampoline, record->name, stack_bytes, record, tskIDLE_PRIORITY + 1,
                                      &record->handle);
    if (created != pdPASS) {
        task__foreground_stack_pop_if_top_locked(record->id);
        task__free_argv(record->argc, record->argv);
        record->in_use = false;
        task__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    *out_task_id = record->id;
    task__unlock();
    return BRUCE_OK;
}

bruce_resource_id_t task_registry__resource_register(bruce_task_resource_cleanup_t cleanup, void *context)
{
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
    if (self->next_resource_id == BRUCE_RESOURCE_ID_INVALID) {
        self->next_resource_id = 1;
    }
    self->resources[free_slot].id = id;
    self->resources[free_slot].cleanup = cleanup;
    self->resources[free_slot].context = context;
    self->resources[free_slot].active = true;
    self->resource_count++;
    task__unlock();
    return id;
}

bruce_result_t task_registry__resource_release(bruce_resource_id_t resource_id)
{
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

void task_registry__account_memory(int64_t delta_bytes)
{
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

/* ---- Public core_sdk/task.h API ---- */

bruce_task_id_t task__current_id(void)
{
    task__ensure_init();
    task__lock();
    task__record_t *self = task__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    bruce_task_id_t id = self != NULL ? self->id : BRUCE_TASK_ID_INVALID;
    task__unlock();
    return id;
}

bruce_result_t task__list(bruce_task_snapshot_t *snapshots, size_t capacity, size_t *out_count)
{
    task__ensure_init();
    if (out_count == NULL || (capacity != 0 && snapshots == NULL)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
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

bruce_result_t task__snapshot(bruce_task_id_t task_id, bruce_task_snapshot_t *out_snapshot)
{
    task__ensure_init();
    if (out_snapshot == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
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

bruce_result_t task__to_background(void)
{
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
    self->state = BRUCE_TASK_BACKGROUND;
    task__wake_locked(self);
    task__foreground_stack_pop_if_top_locked(self->id);
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__foreground(bruce_task_id_t task_id)
{
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
    task__foreground_stack_push_locked(task_id, target);
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__stop(bruce_task_id_t task_id)
{
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
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__pause(bruce_task_id_t task_id)
{
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
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__resume(bruce_task_id_t task_id)
{
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
    record->state = record->state_before_pause;
    task__wake_locked(record);
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__kill(bruce_task_id_t task_id)
{
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

    /* Force-delete another task first so it cannot race the cleanup below
     * (note: if it currently held a non-Core mutex, that mutex is stranded -
     * a known limitation of forceful kill). */
    if (handle != NULL) {
        vTaskDelete(handle);
    }
    task__teardown_locked(record);
    task__unlock();
    return BRUCE_OK;
}

bruce_result_t task__wait(bruce_task_id_t task_id, uint32_t timeout_ms)
{
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

    EventBits_t bits = xEventGroupWaitBits(events, TASK__EVT_EXITED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    task__lock();
    bool recycled = s_tasks[slot].generation != generation;
    task__unlock();

    if (recycled || (bits & TASK__EVT_EXITED) != 0) {
        return BRUCE_OK;
    }
    return BRUCE_ERR_TIMEOUT;
}

/* Shared implementation for runtime__sleep()/runtime__delay(): blocks while
 * paused (until resumed or stopped), returns BRUCE_ERR_CANCELLED as soon as a
 * stop is requested, and otherwise waits out `ms`.  When `interruptible` is
 * true and the task is background when the wait begins, being foregrounded
 * mid-wait also returns BRUCE_ERR_CANCELLED early. */
static bruce_result_t task__wait_ms(uint32_t ms, bool interruptible)
{
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

    TickType_t total_ticks = pdMS_TO_TICKS(ms);
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        task__lock();
        bool stopped = s_tasks[slot].stop_requested;
        bool paused = s_tasks[slot].pause_requested;
        bool now_foreground = s_tasks[slot].state == BRUCE_TASK_FOREGROUND;
        task__unlock();

        if (stopped) {
            return BRUCE_ERR_CANCELLED;
        }
        if (paused) {
            xEventGroupWaitBits(s_task_events[slot], TASK__EVT_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        if (interruptible && was_background && now_foreground) {
            return BRUCE_ERR_CANCELLED;
        }

        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= total_ticks) {
            return BRUCE_OK;
        }
        EventBits_t bits =
            xEventGroupWaitBits(s_task_events[slot], TASK__EVT_WAKE, pdTRUE, pdFALSE, total_ticks - elapsed);
        if (bits == 0) {
            return BRUCE_OK;
        }
        /* Woken early; loop to re-check stop/pause/foreground state. */
    }
}

bruce_result_t runtime__sleep(uint32_t milliseconds)
{
    return task__wait_ms(milliseconds, true);
}

bruce_result_t runtime__delay(uint32_t milliseconds)
{
    return task__wait_ms(milliseconds, false);
}
