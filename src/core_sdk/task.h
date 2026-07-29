#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* Opaque, nonzero Core identifiers.  Applications must not infer any layout
 * or relationship to FreeRTOS handles from their numeric values. */
typedef uint32_t bruce_task_id_t;
typedef uint32_t bruce_resource_id_t;
typedef uint32_t bruce_file_id_t;
typedef uint32_t bruce_viewer_id_t;
typedef uint32_t bruce_tcp_id_t;
typedef uint32_t bruce_i2c_id_t;
typedef uint32_t bruce_spi_id_t;

#define BRUCE_TASK_ID_INVALID ((bruce_task_id_t)0)
#define BRUCE_RESOURCE_ID_INVALID ((bruce_resource_id_t)0)
#define BRUCE_FILE_ID_INVALID ((bruce_file_id_t)0)
#define BRUCE_VIEWER_ID_INVALID ((bruce_viewer_id_t)0)
#define BRUCE_TCP_ID_INVALID ((bruce_tcp_id_t)0)
#define BRUCE_I2C_ID_INVALID ((bruce_i2c_id_t)0)
#define BRUCE_SPI_ID_INVALID ((bruce_spi_id_t)0)

#define BRUCE_TASK_NAME_MAX 64

typedef enum {
    BRUCE_TASK_STARTING = 0,
    BRUCE_TASK_FOREGROUND,
    BRUCE_TASK_BACKGROUND,
    BRUCE_TASK_PAUSED,
    BRUCE_TASK_STOPPING,
} bruce_task_state_t;

/* A point-in-time, read-only description of one live task. */
typedef struct {
    bruce_task_id_t id;
    bruce_task_state_t state;
    char name[BRUCE_TASK_NAME_MAX];
    uint32_t stack_high_water_bytes;
    uint32_t cpu_percent;
    size_t memory_bytes;
    size_t resource_count;
    bool built_in;
    bool gui_requested;
} bruce_task_snapshot_t;

/* All task APIs below return BRUCE_OK or a documented BRUCE_ERR_* result.
 * Foregrounding, switching, stopping, pausing, resuming, or killing another
 * task requires the `task` permission; a task may perform operations on itself. */
bruce_task_id_t task__current_id(void);
bruce_result_t task__list(bruce_task_snapshot_t *snapshots, size_t capacity, size_t *out_count);
bruce_result_t task__snapshot(bruce_task_id_t task_id, bruce_task_snapshot_t *out_snapshot);
bruce_result_t task__switch_next(void);
bruce_result_t task__switch_previous(void);
bruce_result_t task__to_background(void);
/* Declares the calling task GUI-capable and gives it foreground ownership.
 * This self-only operation does not require the `task` permission. */
bruce_result_t task__to_foreground(void);
bruce_result_t task__foreground(bruce_task_id_t task_id);
bruce_result_t task__stop(bruce_task_id_t task_id);
bruce_result_t task__pause(bruce_task_id_t task_id);
bruce_result_t task__resume(bruce_task_id_t task_id);
bruce_result_t task__kill(bruce_task_id_t task_id);
bruce_result_t task__wait(bruce_task_id_t task_id, uint32_t timeout_ms);

/* Monotonic milliseconds since boot.  Use differences between readings for
 * elapsed-time measurement; this is not wall-clock time. */
uint64_t runtime__now(void);

/* Both return BRUCE_OK once the full duration has elapsed and
 * BRUCE_ERR_CANCELLED if interrupted early.  runtime__sleep is interrupted
 * when the calling task is paused (it blocks until resumed, then keeps
 * waiting), stopped, or foregrounded while it was background when the call
 * began.  runtime__delay honours pause/stop the same way but is never
 * interrupted merely by foregrounding; it waits its requested duration. */
bruce_result_t runtime__sleep(uint32_t milliseconds);
bruce_result_t runtime__delay(uint32_t milliseconds);
