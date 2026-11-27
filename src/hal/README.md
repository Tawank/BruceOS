# src/hal

Bruce's hardware layer: a tree of devices described by the board and bound
to drivers, after Linux's device model (buses, drivers, GPIO descriptors,
regulators, deferred probe) and Zephyr's static device definitions.

Only some hardware has moved here so far. Most is still set up from Kconfig
(`src/Kconfig.projbuild`) by the code in `src/core`; it moves over one
device class at a time.

## Pieces

- **Device** (`hal_device_t`, `hal.h`): constant data from the board (name,
  driver, parent bus and address, pins by role, power rails, driver
  settings) plus a small state record (`hal_device_state_t`). A device with
  no driver is just a named set of pins and rails for code in `src/core`
  that still drives the chip itself (`gps`, `modem` on the T-Deck MAX).
- **Driver** (`hal_driver_t`): a name, a class, `probe`, optional
  `suspend`/`resume`, and the class's operations (`api`). It never knows the
  board: everything it needs comes from its device.
- **Classes**: `HAL_CLASS_I2C` (`hal_i2c.h`), `HAL_CLASS_SPI` (`hal_spi.h`),
  `HAL_CLASS_BATTERY` (`hal_battery.h`), `HAL_CLASS_TOUCH` (`hal_touch.h`),
  `HAL_CLASS_KEYPAD` (`hal_keypad.h`), `HAL_CLASS_IMU` (`hal_imu.h`),
  `HAL_CLASS_HAPTIC` (`hal_haptic.h`),
  `HAL_CLASS_GPIO` (`hal_gpio.h`),
  `HAL_CLASS_RAIL` (`hal_rail.h`). More come with the drivers that need
  them (display, audio codec...).
- **Pins** (`hal_pin_t`): a controller and a pin number. The controller is
  NULL for the SoC's GPIO, or any GPIO-class device: an I2C expander pin
  works exactly like an ESP pin. Levels are logical; `HAL_PIN_ACTIVE_LOW`
  flips them.
- **Rails**: a supply switched by a pin, counted on while any powered device
  uses it (`hal_device_power()`). Probing turns it off, unless it has
  `.keep` and is on: after an ESP restart (an expander keeps its outputs)
  the chip behind it keeps running, and its driver takes it from there.
- **Hogs** (`hal_hog.h`): pins held active or inactive from boot, for chips
  with no driver yet or that must stay quiet until theirs runs (Linux's
  GPIO hogs), such as chip selects on a shared SPI bus.

## Startup

`hal__init()` (from `device__power_hold_init()`, early in boot) probes the
board's devices in passes until nothing changes. A device is probed only
once its bus, the controllers of its pins and its rails are ready; until
then it waits (deferred probe), so the board may list devices in any
order. A probe returning `BRUCE_ERR_NOT_FOUND` on a device marked
`.optional` leaves it `absent` rather than `failed`; `BRUCE_ERR_BUSY` asks
to be tried again on the next pass. `hal__suspend()`/`hal__resume()` walk
the ready devices in reverse and forward probe order.

The `hal` shell command lists every device with its driver, bus and state,
plus a line from the driver's optional `describe` (e.g. what a chip
reported at probe).

## A board

`boards/<id>/board.c` is built into the firmware when it exists, else
`board_none.c` (no devices: lookups find nothing, power calls do nothing).
The id is `CONFIG_BRUCE_BOARD_ID`, which a board with a `board.c` sets in
its `sdkconfig.defaults`, so any build directory gets it (`build/` after
`tools/board.py --set-default`, as well as `build-board/<id>`).

```c
static const hal_i2c_master_config_t i2c0_config = {.port = 0, .sda = 13, .scl = 14, .pullup = true};
HAL_DEVICE(i2c0, .driver = &hal_i2c_master_driver, .config = &i2c0_config);
HAL_DEVICE(xl9555, .driver = &xl9555_driver, .bus = &i2c0, .address = 0x20, .config = &xl9555_config);
HAL_RAIL(gps_power, .enable = HAL_PIN(xl9555, 2));
HAL_DEVICE(gps, HAL_RAILS(&gps_power));
HAL_DEVICE(modem, HAL_PINS({"pwrkey", HAL_PIN(xl9555, 8)}), HAL_RAILS(&modem_power));
HAL_BOARD("LilyGO T-Deck MAX", &i2c0, &xl9555, &gps_power, &modem_power, &gps, &modem);
```

Device names are how code finds them, so they name the role (`gps`,
`modem`), not the chip. Moving a pin to another controller, or a supply to
another rail, changes only the board file.

## Buses

`hal_i2c_master_driver` and `hal_spi_master_driver` set up one of the
ESP32's controllers from the board's pins. A chip's driver gets its handle
with `hal_i2c_open()` or `hal_spi_open()` and never names a port, host or
bus pin. Code in `src/core` that still sets up its own chips uses the same
buses: with `CONFIG_BRUCE_BOARD_I2C_HAL`, `board_i2c__acquire()`,
`board_i2c__pins()` and `board_i2c__speed_hz()` give the first HAL I2C bus
and its `hal_i2c_master_config_t` instead of the Kconfig pins. With
`CONFIG_BRUCE_SD_HAL` and `CONFIG_BRUCE_DISPLAY_HAL` (e-paper so far), the
SD card and display take their chip's pins from board.c's driverless `sd`
and `display` devices and the host and bus pins from their bus
(`hal_spi_bus_config()`). The SD
card and display drivers find the SPI host already set up and only add
their chip to it.

## Battery

A battery-class driver (`drivers/battery/`, e.g. `bq27220.c`) reads charge
and whether external power is in. A board that sets
`CONFIG_BRUCE_BATTERY_BACKEND_HAL` gets its level from the first battery
device in `board.c`: the `device_bus` process polls it with
`hal_battery_read()` as it does the Kconfig backends.

## Input

A touch driver (`drivers/touch/`, e.g. `cst3530.c`) reports a point in
screen pixels, turned by its board-given `hal_touch_orientation_t`, and
any touch key held. A keypad driver (`drivers/keypad/`, e.g. `tca8418.c`)
hands out matrix key changes one at a time; what a key means stays with
the keyboard layout in `src/modules/input`. `CONFIG_BRUCE_TOUCH_HAL` and
`CONFIG_BRUCE_KEYBOARD_TCA8418_HAL` make the `device_bus` and input
processes use the board's first touch and keypad devices.

Both drivers pulse their `reset` pin at probe and set the chip up on the
first read, so boot doesn't wait for the chips to start. With an
`interrupt` pin on the SoC, they go to the chip only after it was active
(`hal_pin_irq_enable()`, `hal_pin_irq_take()`), while a finger is down or
key events are left, and once a second in case one was missed. The
keypad's interrupt is level-triggered and masked until taken, so the same
pin wakes light sleep; touch, which only pulses its line, uses the edge.
`hal_pin_irq_set_hook()` lets Core wake its waiting pollers. Otherwise every read
goes to the chip.

## LEDs

An LED-class driver sets a brightness from 0 to 255 (`hal_led_set()`).
`pwm_led_driver` (`drivers/led/pwm_led.c`, like Linux's leds-pwm) dims
the SoC pin named `led` with an LEDC timer and channel that board.c gives
it. Core's `device__set_keyboard_light()` drives the device named
`keyboard_light`, at its saved level from boot and off while the device
sleeps.

## Using a device

```c
const hal_device_t *modem = hal_device_find("modem");   /* NULL: the board has none */
hal_device_power(modem, true);                           /* its rails on */
hal_pin_set(hal_device_pin(modem, "pwrkey"), true);      /* wherever that pin is */
```

Each call accepts NULL and does nothing, so code works on boards without
the device.

A driver with `start`/`stop` is brought up on demand: `hal_device_get()`
takes a user (the first runs `start`), `hal_device_put()` drops one (the
last runs `stop`), after Linux's runtime PM. `hal start|stop <device>` does
the same from the shell. The BHI260AP uses it to upload its firmware from
`/lib/firmware/bhi260ap.fw` (or the SD card's `/lib/firmware`; the file is
in `sd_files/lib/firmware`) only when something needs the sensor.

## A driver

One file under `drivers/<class>/`, with a header giving its settings struct
and `extern const hal_driver_t <chip>_driver;`. `probe` gets everything
from the device: `hal_i2c_open(device, ...)` for its bus handle,
`hal_device_pin(device, "reset")` for pins, `device->config` for settings,
and keeps its own state in `device->state->data`. A board only links the
drivers it names, so an unused driver costs no flash.
