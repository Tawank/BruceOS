#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Process lifecycle: listing, foreground/background switching, signaling, pause/resume.
 *
 * All process APIs below return BRUCE_OK or a documented BRUCE_ERR_*
 * result. Foregrounding, switching, signalling, pausing, resuming, or
 * killing *another* process requires the `process` permission; a process
 * may always perform these operations on itself.
 */

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

bruce_process_id_t process__current_id(void);

/**
 * @brief Returns the pending cooperative INT/TERM signal for the calling process.
 *
 * Zero when no cooperative signal is pending.
 */
bruce_process_signal_t process__current_signal(void);

/**
 * @brief Lists live processes.
 *
 * @param snapshots Array to receive process snapshots.
 * @param capacity Number of entries the snapshots array can hold.
 * @param out_count Receives the total number of live processes.
 */
bruce_result_t process__list(bruce_process_snapshot_t *snapshots, size_t capacity, size_t *out_count);

/**
 * @brief Reads a point-in-time snapshot of one process.
 *
 * @param process_id Process to snapshot.
 * @param out_snapshot Receives the snapshot.
 */
bruce_result_t process__snapshot(bruce_process_id_t process_id, bruce_process_snapshot_t *out_snapshot);

/**
 * @brief Switches the foreground process to the next one in launch order.
 *
 * @permission process
 */
bruce_result_t process__switch_next(void);

/**
 * @brief Switches the foreground process to the previous one in launch order.
 *
 * @permission process
 */
bruce_result_t process__switch_previous(void);

/** @brief Moves the calling process to the background. Self-only. */
bruce_result_t process__to_background(void);

/**
 * @brief Declares the calling process GUI-capable and gives it foreground ownership.
 *
 * @permission none (self-only operation)
 */
bruce_result_t process__to_foreground(void);

/**
 * @brief Gives another process foreground ownership.
 *
 * @param process_id Process to foreground.
 * @permission process
 */
bruce_result_t process__foreground(bruce_process_id_t process_id);

/**
 * @brief Sends a signal to a process.
 *
 * INT and TERM are cooperative; KILL is forced and equivalent to
 * process__kill().
 *
 * @param process_id Process to signal.
 * @param signal Signal to send.
 * @permission process
 */
bruce_result_t process__signal(bruce_process_id_t process_id, bruce_process_signal_t signal);

/**
 * @brief Sends a cooperative TERM signal to a process.
 *
 * @param process_id Process to terminate.
 * @permission process
 */
bruce_result_t process__terminate(bruce_process_id_t process_id);

/**
 * @brief Pauses a process.
 *
 * @param process_id Process to pause.
 * @permission process
 */
bruce_result_t process__pause(bruce_process_id_t process_id);

/**
 * @brief Resumes a paused process.
 *
 * @param process_id Process to resume.
 * @permission process
 */
bruce_result_t process__resume(bruce_process_id_t process_id);

/**
 * @brief Forcibly kills a process, equivalent to process__signal() with BRUCE_PROCESS_SIGNAL_KILL.
 *
 * @param process_id Process to kill.
 * @permission process
 */
bruce_result_t process__kill(bruce_process_id_t process_id);

/**
 * @brief Non-consuming wait for a process to complete.
 *
 * Succeeds for either a live process that completes before the timeout or
 * an already-retained completion. UINT32_MAX waits forever and zero polls.
 *
 * @param process_id Process to wait for.
 * @param timeout_ms Maximum time to wait, in milliseconds (UINT32_MAX waits forever, 0 polls).
 * @permission none
 */
bruce_result_t process__wait(bruce_process_id_t process_id, uint32_t timeout_ms);

/**
 * @brief Atomically consumes one retained completion.
 *
 * Exactly one concurrent caller can succeed. UINT32_MAX waits forever and
 * zero polls. On failure, *out_status is unchanged.
 *
 * @param process_id Process to wait for.
 * @param timeout_ms Maximum time to wait, in milliseconds (UINT32_MAX waits forever, 0 polls).
 * @param out_status Receives the process's completion status.
 * @permission process
 */
bruce_result_t
process__wait_status(bruce_process_id_t process_id, uint32_t timeout_ms, bruce_process_status_t *out_status);

#ifdef __cplusplus
}
#endif
