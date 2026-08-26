#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque, nonzero Core identifiers.  Applications must not infer any layout
 * or relationship to FreeRTOS handles from their numeric values. */
typedef uint32_t bruce_process_id_t;
typedef uint32_t bruce_resource_id_t;
typedef uint32_t bruce_file_id_t;
typedef uint32_t bruce_viewer_id_t;
typedef uint32_t bruce_tcp_id_t;
typedef uint32_t bruce_i2c_id_t;
typedef uint32_t bruce_spi_id_t;
typedef uint32_t bruce_ssh_id_t;
typedef uint32_t bruce_pubsub_id_t;

#define BRUCE_PROCESS_ID_INVALID ((bruce_process_id_t)0)
#define BRUCE_RESOURCE_ID_INVALID ((bruce_resource_id_t)0)
#define BRUCE_FILE_ID_INVALID ((bruce_file_id_t)0)
#define BRUCE_VIEWER_ID_INVALID ((bruce_viewer_id_t)0)
#define BRUCE_TCP_ID_INVALID ((bruce_tcp_id_t)0)
#define BRUCE_I2C_ID_INVALID ((bruce_i2c_id_t)0)
#define BRUCE_SPI_ID_INVALID ((bruce_spi_id_t)0)
#define BRUCE_SSH_ID_INVALID ((bruce_ssh_id_t)0)
#define BRUCE_PUBSUB_ID_INVALID ((bruce_pubsub_id_t)0)

#define BRUCE_PROCESS_NAME_MAX 64

typedef enum {
    BRUCE_PROCESS_STARTING = 0,
    BRUCE_PROCESS_FOREGROUND,
    BRUCE_PROCESS_BACKGROUND,
    BRUCE_PROCESS_PAUSED,
    BRUCE_PROCESS_STOPPING,
} bruce_process_state_t;

typedef enum {
    BRUCE_PROCESS_EXITED = 0,
    BRUCE_PROCESS_TERMINATED,
    BRUCE_PROCESS_KILLED,
} bruce_process_end_reason_t;

typedef enum {
    BRUCE_PROCESS_SIGNAL_INT = 2,
    BRUCE_PROCESS_SIGNAL_KILL = 9,
    BRUCE_PROCESS_SIGNAL_TERM = 15,
} bruce_process_signal_t;

typedef struct {
    bruce_process_end_reason_t reason;
    int exit_code;
    /* Meaningful for TERMINATED/KILLED; zero for EXITED. */
    bruce_process_signal_t signal;
} bruce_process_status_t;

/* A point-in-time, read-only description of one live process. */
typedef struct {
    bruce_process_id_t id;
    bruce_process_state_t state;
    char name[BRUCE_PROCESS_NAME_MAX];
    uint32_t stack_high_water_bytes;
    uint32_t stack_total_bytes;
    uint32_t cpu_percent;
    size_t memory_bytes;
    size_t swap_bytes;
    size_t resource_count;
    bool built_in;
    bool gui_requested;
    bool presentable;
} bruce_process_snapshot_t;

/* All process APIs below return BRUCE_OK or a documented BRUCE_ERR_* result.
 * Foregrounding, switching, signalling, pausing, resuming, or killing another
 * process requires the `process` permission; a process may perform operations on itself. */
bruce_process_id_t process__current_id(void);
/* Returns the pending cooperative INT/TERM signal for the calling process, or
 * zero when no cooperative signal is pending. */
bruce_process_signal_t process__current_signal(void);
bruce_result_t process__list(bruce_process_snapshot_t *snapshots, size_t capacity, size_t *out_count);
bruce_result_t process__snapshot(bruce_process_id_t process_id, bruce_process_snapshot_t *out_snapshot);
bruce_result_t process__switch_next(void);
bruce_result_t process__switch_previous(void);
bruce_result_t process__to_background(void);
/* Declares the calling process GUI-capable and gives it foreground ownership.
 * This self-only operation does not require the `process` permission. */
bruce_result_t process__to_foreground(void);
bruce_result_t process__foreground(bruce_process_id_t process_id);
/* INT and TERM are cooperative; KILL is forced and equivalent to process__kill(). */
bruce_result_t process__signal(bruce_process_id_t process_id, bruce_process_signal_t signal);
bruce_result_t process__terminate(bruce_process_id_t process_id);
bruce_result_t process__pause(bruce_process_id_t process_id);
bruce_result_t process__resume(bruce_process_id_t process_id);
bruce_result_t process__kill(bruce_process_id_t process_id);
/* Non-consuming: succeeds for either a live process that completes before the
 * timeout or an already-retained completion. UINT32_MAX waits forever and zero
 * polls. No process permission is required. */
bruce_result_t process__wait(bruce_process_id_t process_id, uint32_t timeout_ms);
/* Atomically consumes one retained completion. Exactly one concurrent caller
 * can succeed; the `process` permission is required. UINT32_MAX waits forever
 * and zero polls. On failure, *out_status is unchanged. */
bruce_result_t
process__wait_status(bruce_process_id_t process_id, uint32_t timeout_ms, bruce_process_status_t *out_status);

#ifdef __cplusplus
}
#endif
