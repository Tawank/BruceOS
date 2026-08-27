#pragma once

#include "process.h"

#include "core_sdk/permission.h"
#include "core_sdk/stdio.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/event_groups.h"
#include "freertos/task.h"

typedef struct {
    char *name;
    char *value;
} process__environment_entry_t;

typedef struct {
    process__environment_entry_t *entries;
    size_t count;
    size_t capacity;
} process__environment_t;

typedef struct process__resource {
    bruce_resource_id_t id;
    bruce_process_resource_cleanup_t cleanup;
    void *context;
    struct process__resource *next;
} process__resource_t;

typedef struct process__record {
    bool in_use;
    bruce_process_id_t id;
    char name[BRUCE_PROCESS_NAME_MAX];
    bruce_process_state_t state;
    bool built_in;
    bool gui_requested;
    bool presentable;
    char permission_key[BRUCE_PERMISSION_FILE_NAME_MAX];
    bool start_in_background;
    bool preserve_display;
    bruce_stdio_session_t stdio_session;
    bruce_stdio_session_t child_stdio_session;
    TaskHandle_t handle;

    bruce_app_entry_t entry;
    int argc;
    char **argv;

    int (*process_entry)(void *context);
    void *process_entry_context;
    void (*process_entry_cleanup)(void *context);
    void (*process_entry_stop)(void *context, bruce_process_signal_t signal);
    volatile bool stop_requested;
    bruce_process_signal_t pending_signal;
    /* Pins the owned context while a stop hook runs outside the registry lock. */
    size_t stop_callback_count;
    bool teardown_pending;
    bruce_process_status_t pending_status;
    volatile bool pause_requested;
    size_t waiter_count;
    size_t status_waiter_count;
    bool wait_attached;
    struct process__record *wait_target;
    bool wait_for_status;

    process__resource_t *resources;
    bruce_resource_id_t next_resource_id;
    size_t resource_count;
    size_t memory_bytes;
    size_t swap_bytes;
    size_t operation_count;

    uint32_t last_runtime_counter;
    uint32_t cpu_percent;
    uint32_t stack_high_water_bytes;
    uint32_t stack_total_bytes;
    process__environment_t environment;
    EventGroupHandle_t events;
    struct process__record *previous;
    struct process__record *next;
    struct process__record *fg_previous;
    struct process__record *fg_next;
} process__record_t;

typedef struct process__completion {
    bool in_use;
    bruce_process_id_t id;
    bruce_process_status_t status;
    uint64_t sequence;
    size_t waiter_pins;
    struct process__completion *next;
} process__completion_t;

extern process__record_t *s_processes;
extern process__record_t *s_process_tail;
extern process__environment_t s_global_environment;
extern process__record_t *s_fg_tail;
extern bruce_process_id_t s_effective_foreground;

#define PROCESS__EVT_WAKE (1u << 0)
#define PROCESS__EVT_EXITED (1u << 1)
#define PROCESS__EVT_EVENT_WAKE (1u << 2)
#define PROCESS__EVT_WAITER_WAKE (1u << 3)
#define PROCESS__EVT_OPERATION_IDLE (1u << 4)
#define PROCESS__EVT_STOP_CALLBACK_IDLE (1u << 5)
#define PROCESS__TLS_SLOT 1

void process__ensure_init(void);
void process__lock(void);
void process__unlock(void);
process__record_t *process__find_by_id_locked(bruce_process_id_t id);
process__record_t *process__find_by_handle_locked(TaskHandle_t handle);
process__record_t *process__current_record(void);
process__completion_t *process__find_completion_locked(bruce_process_id_t id);
void process__completion_clear_locked(process__completion_t *completion);
void process__detach_wait_locked(process__record_t *waiter);
void process__dispose_if_unused_locked(process__record_t *record);
void process__wake_locked(process__record_t *record);
void process__foreground_recompute_locked(void);
void process__foreground_remove_locked(bruce_process_id_t id);
void process__foreground_push_locked(bruce_process_id_t id);
void process__refresh_cpu_samples_locked(void);
void process__fill_snapshot_locked(const process__record_t *record, bruce_process_snapshot_t *out_snapshot);
void process__teardown_locked(process__record_t *record, const bruce_process_status_t *status);

bool process__environment_name_valid(const char *name);
int process__environment_find(const process__environment_t *environment, const char *name);
void process__environment_free(process__environment_t *environment);
bruce_result_t
process__environment_set_locked(process__environment_t *environment, const char *name, const char *value);
bruce_result_t process__environment_inherit_locked(
    process__record_t *record, const process__record_t *parent, const bruce_environment_variable_t *overlay,
    size_t overlay_count
);
