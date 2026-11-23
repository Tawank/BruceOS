#pragma once

/* Bruce's hardware layer: a tree of devices described by the board, each
 * bound to a driver, after Linux's device model and Zephyr's static devices.
 *
 * A board (boards/<id>/board.c) lists its devices as constant data: buses,
 * GPIO controllers (the SoC and expanders such as the XL9555), power rails
 * and chips. hal__init() probes them parents first, putting off any whose
 * bus, pin controller or rail isn't ready yet (Linux's deferred probe).
 * Code that uses a device finds it by name and asks for pins by role
 * ("pwrkey") and for its power, without knowing how the board wires them:
 * a pin may be an ESP GPIO or a pin of an I2C expander, a rail an expander
 * output, all decided by the board file. See src/hal/README.md. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

typedef struct hal_device hal_device_t;

/* What a driver provides; `api` points at that class's operations. */
typedef enum {
    HAL_CLASS_NONE, /* A device with no driver: pins and rails for code that drives it itself. */
    HAL_CLASS_I2C,
    HAL_CLASS_SPI,
    HAL_CLASS_GPIO,
    HAL_CLASS_RAIL,
    HAL_CLASS_COUNT,
} hal_class_t;

typedef enum {
    HAL_STATE_PENDING, /* Not probed yet, or put off until what it needs is ready. */
    HAL_STATE_READY,
    HAL_STATE_FAILED,
    HAL_STATE_ABSENT, /* Optional and not found. */
    HAL_STATE_SUSPENDED,
} hal_state_t;

/* ---- Pins ------------------------------------------------------------------- */

#define HAL_PIN_ACTIVE_LOW 0x01u

/* A pin on a GPIO controller; `controller` NULL is the SoC's own GPIO.
 * Levels are logical: "active" is low for HAL_PIN_ACTIVE_LOW. */
typedef struct {
    const hal_device_t *controller;
    uint8_t number;
    uint8_t flags;
} hal_pin_t;

/* A pin a device uses, by role. */
typedef struct {
    const char *role;
    hal_pin_t pin;
} hal_pin_map_t;

/* Initializers for board data. */
#define HAL_SOC_PIN(gpio) {.controller = NULL, .number = (gpio)}
#define HAL_SOC_PIN_LOW(gpio) {.controller = NULL, .number = (gpio), .flags = HAL_PIN_ACTIVE_LOW}
#define HAL_PIN(chip, n) {.controller = &(chip), .number = (n)}
#define HAL_PIN_LOW(chip, n) {.controller = &(chip), .number = (n), .flags = HAL_PIN_ACTIVE_LOW}

/* ---- Devices and drivers --------------------------------------------------- */

typedef struct {
    const char *name;
    hal_class_t class_id;
    /* Brings the device up; BRUCE_ERR_NOT_FOUND when an optional chip isn't there. */
    bruce_result_t (*probe)(const hal_device_t *device);
    bruce_result_t (*suspend)(const hal_device_t *device);
    bruce_result_t (*resume)(const hal_device_t *device);
    const void *api;
} hal_driver_t;

/* Changes at run time; one per device, next to it. */
typedef struct {
    hal_state_t state;
    bruce_result_t error; /* Why it failed or is absent. */
    bool powered;         /* hal_device_power() holds its rails. */
    int16_t users;        /* Rails: devices holding it on. */
    void *data;           /* The driver's own. */
} hal_device_state_t;

struct hal_device {
    const char *name;
    const hal_driver_t *driver; /* NULL: HAL_CLASS_NONE. */
    const hal_device_t *bus;    /* Parent bus, if any. */
    uint16_t address;           /* On that bus: I2C address, SPI chip... */
    const hal_pin_map_t *pins;
    uint8_t pin_count;
    const hal_device_t *const *rails; /* Supplies hal_device_power() turns on. */
    uint8_t rail_count;
    bool optional; /* Missing is not an error. */
    const void *config; /* The driver's settings from the board. */
    hal_device_state_t *state;
};

/* Defines a device `id` with its state. Fields as in hal_device_t, e.g.
 * HAL_DEVICE(gps, .rails = HAL_RAILS(&gps_power)). */
#define HAL_DEVICE(id, ...)                                                                       \
    static hal_device_state_t id##__state;                                                        \
    static const hal_device_t id = {.name = #id, .state = &id##__state, __VA_ARGS__}

#define HAL_PINS(...)                                                                             \
    .pins = (const hal_pin_map_t[]){__VA_ARGS__},                                                 \
    .pin_count = sizeof((const hal_pin_map_t[]){__VA_ARGS__}) / sizeof(hal_pin_map_t)
#define HAL_RAILS(...)                                                                            \
    .rails = (const hal_device_t *const[]){__VA_ARGS__},                                          \
    .rail_count = sizeof((const hal_device_t *const[]){__VA_ARGS__}) / sizeof(hal_device_t *)

/* The board: every device, in any order. */
typedef struct {
    const char *name;
    const hal_device_t *const *devices;
    size_t device_count;
} hal_board_t;

#define HAL_BOARD(board_name, ...)                                                                \
    static const hal_device_t *const hal_board__devices[] = {__VA_ARGS__};                        \
    const hal_board_t hal_board = {.name = (board_name),                                          \
                                   .devices = hal_board__devices,                                 \
                                   .device_count = sizeof(hal_board__devices) / sizeof(hal_board__devices[0])}

extern const hal_board_t hal_board;

/* ---- Using devices --------------------------------------------------------- */

/* Probes the board's devices. Call once, early in boot. */
void hal__init(void);

/* NULL when the board has no such device. */
const hal_device_t *hal_device_find(const char *name);
/* The first ready device of a class. */
const hal_device_t *hal_device_first(hal_class_t class_id);
bool hal_device_ready(const hal_device_t *device);
/* A device's pin by role; NULL when the device or the pin is missing. */
const hal_pin_t *hal_device_pin(const hal_device_t *device, const char *role);
/* Holds or lets go of the device's rails. No-op for a NULL device or one without rails. */
bruce_result_t hal_device_power(const hal_device_t *device, bool on);

/* Suspends every ready device, children first, and resumes them in the opposite order. */
void hal__suspend(void);
void hal__resume(void);

/* Calls `visit` for each device, in board order. */
void hal__for_each(void (*visit)(const hal_device_t *device, void *context), void *context);
const char *hal__state_name(hal_state_t state);
