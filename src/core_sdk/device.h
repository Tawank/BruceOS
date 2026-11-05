#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/input.h"
#include "core_sdk/result.h"

/**
 * @brief Battery, power, and device sensors.
 */

/**
 * @brief Returns battery charge from 0 through 100, or a negative BRUCE_ERR_* value.
 *
 * Analog-only boards estimate charge from battery voltage.
 */
int device__get_battery(void);

/**
 * @brief Schedules a device restart after delay_ms.
 *
 * @param delay_ms Delay before restarting, in milliseconds.
 * @permission process
 */
bruce_result_t device__restart(uint32_t delay_ms);

/**
 * @brief Schedules device power-off after delay_ms.
 *
 * Releases a configured power-hold latch, then enters deep sleep. Boards
 * without a latch enter deep sleep directly.
 *
 * @param delay_ms Delay before powering off, in milliseconds.
 * @permission process
 */
bruce_result_t device__power_off(uint32_t delay_ms);

/**
 * @brief Shared board I2C sensor topics (touch, battery).
 *
 * On boards with a touchscreen or an I2C fuel-gauge/PMIC battery backend,
 * the device_bus built-in process owns the shared board I2C bus (see
 * core/device/board_i2c.h) and is the only thing that ever talks to those
 * controllers directly; it publishes their readings on these topics (via
 * core_sdk/pubsub.h) instead. device__get_battery() already reads from the
 * battery topic's cache for you - subscribe to BRUCE_DEVICE_TOPIC_BATTERY
 * yourself only if you need updates as they happen.
 */
#define BRUCE_DEVICE_TOPIC_TOUCH "device.touch"
#define BRUCE_DEVICE_TOPIC_BATTERY "device.battery"

/* x/y are panel pixel coordinates, already adjusted for the board's
 * configured swap/mirror settings. */
typedef struct {
    bruce_input_action_t action; /* BRUCE_INPUT_PRESS/RELEASE/CHANGE */
    int32_t x;
    int32_t y;
} bruce_device_touch_message_t;

typedef struct {
    int32_t percent; /* 0-100 */
} bruce_device_battery_message_t;

/**
 * @brief Crash core dump saved to the `coredump` flash partition.
 *
 * When the firmware is built with CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH, a
 * panic saves the state of every task to flash before rebooting; these
 * functions read it back on the next boot. Builds without core dump support
 * return BRUCE_ERR_UNSUPPORTED from all of them.
 */
#define BRUCE_DEVICE_COREDUMP_TASK_NAME_MAX 16
#define BRUCE_DEVICE_COREDUMP_BACKTRACE_MAX 16
#define BRUCE_DEVICE_COREDUMP_SHA256_MAX 65
#define BRUCE_DEVICE_COREDUMP_REASON_MAX 200

typedef struct {
    char task[BRUCE_DEVICE_COREDUMP_TASK_NAME_MAX];  /* task that crashed */
    uint32_t pc;                                     /* program counter at the crash */
    uint32_t backtrace[BRUCE_DEVICE_COREDUMP_BACKTRACE_MAX];
    uint32_t backtrace_depth;
    bool backtrace_corrupted;
    uint32_t exception_cause;                        /* Xtensa EXCCAUSE; 0 on other targets */
    uint32_t exception_address;                      /* Xtensa EXCVADDR; 0 on other targets */
    char app_elf_sha256[BRUCE_DEVICE_COREDUMP_SHA256_MAX]; /* prefix of the crashed firmware's ELF SHA256 */
    char reason[BRUCE_DEVICE_COREDUMP_REASON_MAX];   /* panic reason, e.g. the failed assert; may be empty */
} bruce_device_coredump_summary_t;

/**
 * @brief Returns the size in bytes of the saved core dump image.
 *
 * @param out_size Receives the image size.
 * @return BRUCE_OK, BRUCE_ERR_NOT_FOUND when no valid dump is saved, or
 *         BRUCE_ERR_UNSUPPORTED when core dumps are disabled in this build.
 * @permission process
 */
bruce_result_t device__coredump_get_size(size_t *out_size);

/**
 * @brief Decodes the crashed task, PC, backtrace, and panic reason from the saved core dump.
 *
 * @param out_summary Receives the summary.
 * @return BRUCE_OK, BRUCE_ERR_NOT_FOUND when no valid dump is saved, or
 *         BRUCE_ERR_UNSUPPORTED when core dumps are disabled in this build.
 * @permission process
 */
bruce_result_t device__coredump_get_summary(bruce_device_coredump_summary_t *out_summary);

/**
 * @brief Reads raw bytes of the saved core dump image.
 *
 * The full image (offset 0 to device__coredump_get_size()) is what
 * `idf.py coredump-info --core-format raw` expects.
 *
 * @param offset Byte offset into the image.
 * @param buffer Destination buffer.
 * @param size Number of bytes to read; offset + size must not exceed the image size.
 * @permission process
 */
bruce_result_t device__coredump_read(size_t offset, void *buffer, size_t size);

/**
 * @brief Erases the saved core dump.
 *
 * @permission process
 */
bruce_result_t device__coredump_erase(void);
