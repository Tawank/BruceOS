#include "gpio_bus_test.h"

#include <stdio.h>

#include "core/permission/permission.h"
#include "core/task/task.h"
#include "core_sdk/gpio.h"
#include "core_sdk/i2c.h"
#include "core_sdk/permission.h"
#include "core_sdk/spi.h"
#include "core_sdk/task.h"

static volatile bruce_result_t s_gpio_bus_result;
static volatile bool s_gpio_bus_ran;

static int selftest__gpio_bus_external_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int level = 0;
    s_gpio_bus_result = gpio__read(0, &level);
    s_gpio_bus_ran = true;
    return 0;
}

bool selftest__run_gpio_bus_permission_denied_case(void) {
    permission__test_reset();
    permission__set("gpio_denied.js", BRUCE_PERMISSION_GPIO, false);
    s_gpio_bus_result = BRUCE_OK;
    s_gpio_bus_ran = false;
    task_create_params_t params = {
        .name = "selftest_gpio",
        .entry = selftest__gpio_bus_external_entry,
        .built_in = false,
        .permission_key = "gpio_denied.js",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_task_id_t id = BRUCE_TASK_ID_INVALID;
    if (task_registry__create(&params, &id) != BRUCE_OK) return false;
    bruce_result_t wait = task__wait(id, 5000);
    bool ok = (wait == BRUCE_OK || wait == BRUCE_ERR_NOT_FOUND) && s_gpio_bus_ran &&
              s_gpio_bus_result == BRUCE_ERR_PERMISSION;
    printf("[selftest] gpio-bus/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", s_gpio_bus_result);
    return ok;
}

bool selftest__run_gpio_bus_validation_case(void) {
    bruce_i2c_id_t i2c_bus = 123;
    bruce_spi_id_t spi_device = 123;
    bruce_i2c_bus_config_t i2c_config = {
        .port = BRUCE_I2C_PORT_AUTO,
        .sda = -1,
        .scl = -1,
        .clock_hz = 400000,
    };
    bruce_spi_device_config_t spi_config = {
        .sck = -1,
        .miso = -1,
        .mosi = -1,
        .cs = -1,
        .clock_hz = 1000000,
    };
    uint8_t byte = 0;
    int level = 0;
    bool present = false;
    bool ok =
        gpio__configure(-1, BRUCE_GPIO_MODE_INPUT, BRUCE_GPIO_PULL_NONE) == BRUCE_ERR_INVALID_ARGUMENT &&
        gpio__read(-1, &level) == BRUCE_ERR_INVALID_ARGUMENT &&
        gpio__read(0, NULL) == BRUCE_ERR_INVALID_ARGUMENT &&
        gpio__write(-1, 0) == BRUCE_ERR_INVALID_ARGUMENT && gpio__write(0, 2) == BRUCE_ERR_INVALID_ARGUMENT &&
        i2c__open(NULL, &i2c_bus) == BRUCE_ERR_INVALID_ARGUMENT &&
        i2c__open(&i2c_config, &i2c_bus) == BRUCE_ERR_INVALID_ARGUMENT && i2c_bus == BRUCE_I2C_ID_INVALID &&
        i2c__probe(BRUCE_I2C_ID_INVALID, 0x50, 10, &present) == BRUCE_ERR_NOT_FOUND &&
        i2c__write(BRUCE_I2C_ID_INVALID, 0x50, NULL, 1, 10) == BRUCE_ERR_INVALID_ARGUMENT &&
        i2c__read(BRUCE_I2C_ID_INVALID, 0x50, &byte, 0, 10) == BRUCE_ERR_INVALID_ARGUMENT &&
        spi__open(NULL, &spi_device) == BRUCE_ERR_INVALID_ARGUMENT &&
        spi__open(&spi_config, &spi_device) == BRUCE_ERR_INVALID_ARGUMENT &&
        spi_device == BRUCE_SPI_ID_INVALID &&
        spi__transfer(BRUCE_SPI_ID_INVALID, &byte, NULL, 0) == BRUCE_ERR_INVALID_ARGUMENT;
    printf("[selftest] gpio-bus/validation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
