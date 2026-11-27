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
 * @brief Whether external power is connected (charging, or full and plugged in).
 *
 * Read with the battery level (every 5 s), from the board's fuel gauge or
 * power chip; false on boards that only measure the battery voltage.
 */
bool device__is_charging(void);

/** What is plugged into the USB port, as the charger chip sees it. */
typedef enum {
    BRUCE_POWER_INPUT_UNKNOWN,           /**< Not read yet. */
    BRUCE_POWER_INPUT_NONE,              /**< Nothing plugged in. */
    BRUCE_POWER_INPUT_USB_HOST,          /**< A computer's USB port (up to 500 mA). */
    BRUCE_POWER_INPUT_USB_CHARGING_HOST, /**< A computer port that charges (up to 1.5 A). */
    BRUCE_POWER_INPUT_USB_CHARGER,       /**< A wall charger. */
    BRUCE_POWER_INPUT_FAST_CHARGER,      /**< A charger that can raise its voltage (HVDCP). */
    BRUCE_POWER_INPUT_ADAPTER,           /**< Another power supply. */
    BRUCE_POWER_INPUT_OTG,               /**< The device powers what is plugged in. */
} bruce_power_input_t;

typedef enum {
    BRUCE_CHARGE_UNKNOWN,
    BRUCE_CHARGE_NOT_CHARGING, /**< Unplugged, or plugged in and not charging. */
    BRUCE_CHARGE_PRECHARGE,    /**< A deeply discharged battery, at a low current. */
    BRUCE_CHARGE_CHARGING,
    BRUCE_CHARGE_FULL,
} bruce_charge_state_t;

typedef struct {
    bruce_power_input_t input;
    bool power_good; /**< The input is usable, not just present. */
    bruce_charge_state_t charge;
    int32_t input_mv;  /**< USB port voltage, or -1 when not measured. */
    int32_t charge_ma; /**< Current into the battery, or -1 when not measured. */
} bruce_device_power_t;

/**
 * @brief The USB input and charging state from the board's charger chip.
 *
 * The status is read every second; the voltage and current are measured
 * with the battery level (every 5 s), only while something is plugged in.
 *
 * @param out Filled in.
 * @return BRUCE_ERR_UNSUPPORTED on boards without a charger chip;
 *         BRUCE_ERR_NOT_FOUND until it has been read.
 */
bruce_result_t device__get_power(bruce_device_power_t *out);

typedef enum {
    BRUCE_USB_HOST_OFF,      /**< The port is the USB serial console. */
    BRUCE_USB_HOST_WAITING,   /**< Powering the port, waiting for a device. */
    BRUCE_USB_HOST_CONNECTED, /**< A supported device is plugged in and feeds input. */
} bruce_usb_host_state_t;

/**
 * @brief Powers the USB port from the battery and takes input from a USB device.
 *
 * Supported so far: boot-protocol keyboards. The port stops being the USB
 * serial console. It goes back, and the power off, when the device is
 * unplugged, when none is plugged in within 15 s, or on device__usb_host_stop().
 *
 * @return BRUCE_ERR_BUSY while a computer or charger is plugged in;
 *         BRUCE_ERR_UNSUPPORTED on boards that can't power the port.
 */
bruce_result_t device__usb_host_start(void);

/** @brief Ends device__usb_host_start() at once. */
bruce_result_t device__usb_host_stop(void);

bruce_usb_host_state_t device__usb_host_state(void);

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
 * @brief Puts the device to sleep until the Select button is pressed.
 *
 * Blocks until wake-up. Waits for the button to be released first; then the
 * backlight is off and input is dropped. On boards with automatic light
 * sleep, other processes keep running and drawing (an e-paper panel keeps
 * updating): the chip sleeps while every task waits and wakes when a delay
 * or timer is due, and device__wake() turns the screen back on. Elsewhere
 * everything pauses, and an e-paper panel keeps showing what it shows now
 * (drawing still in progress is not shown -- see display__wait_shown()).
 *
 * @return BRUCE_ERR_UNSUPPORTED on boards without a Select button.
 * @permission process
 */
bruce_result_t device__sleep(void);

/**
 * @brief Ends device__sleep() as if Select was pressed, e.g. for an alarm.
 *
 * Does nothing while the device is awake.
 */
bruce_result_t device__wake(void);

/**
 * @brief Keeps the device from sleeping when idle, or lets it again.
 *
 * The screen still dims, and on boards with automatic light sleep the chip
 * still sleeps between the process's delays, so its drawing goes on. Cleared
 * when the process ends.
 *
 * @param prevent true to hold the device awake.
 * @return BRUCE_ERR_RESOURCE_LIMIT when too many processes hold it.
 */
bruce_result_t device__prevent_sleep(bool prevent);

/** @brief Whether the board has a keyboard backlight. */
bool device__has_keyboard_light(void);

/**
 * @brief Sets the keyboard backlight and saves it as the setting.
 *
 * The light is off while the device sleeps.
 *
 * @param percent 0 (off) to 100.
 * @return BRUCE_ERR_UNSUPPORTED on boards without one.
 * @permission config
 */
bruce_result_t device__set_keyboard_light(int percent);

/**
 * @brief Shared board I2C sensor topics (touch, battery).
 *
 * On boards with a touchscreen or an I2C fuel-gauge/PMIC battery backend,
 * the device_bus built-in process owns the shared board I2C bus (see
 * core/device/board_i2c.h) and is the only thing that ever talks to those
 * controllers directly; it publishes their readings on these topics (via
 * core_sdk/pubsub.h) instead. device__get_battery() already reads from the
 * latest reading for you - subscribe to BRUCE_DEVICE_TOPIC_BATTERY
 * yourself only if you need updates as they happen.
 */
#define BRUCE_DEVICE_TOPIC_TOUCH "device.touch"
#define BRUCE_DEVICE_TOPIC_BATTERY "device.battery"

/* x/y are panel pixel coordinates, already adjusted for the board's
 * configured swap/mirror settings. A message with key >= 0 is instead a
 * touch key below the screen (0 left, 1 middle, 2 right) and has no x/y. */
typedef struct {
    bruce_input_action_t action; /* BRUCE_INPUT_PRESS/RELEASE/CHANGE */
    int32_t x;
    int32_t y;
    int32_t key;
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
 * Succeeds even when the dump's checksum is invalid, so a partially written
 * dump can still be exported with device__coredump_read().
 *
 * @return BRUCE_OK, BRUCE_ERR_NOT_FOUND when the partition is blank,
 *         BRUCE_ERR_INVALID_STATE when the dump header is damaged, or
 *         BRUCE_ERR_UNSUPPORTED when core dumps are disabled in this build.
 * @permission process
 */
bruce_result_t device__coredump_get_size(size_t *out_size);

/**
 * @brief Decodes the crashed task, PC, backtrace, and panic reason from the saved core dump.
 *
 * @param out_summary Receives the summary.
 * @return BRUCE_OK, BRUCE_ERR_NOT_FOUND when the partition is blank,
 *         BRUCE_ERR_INVALID_STATE when the dump is damaged or its checksum fails, or
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
