#include "board_i2c.h"

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "hal/hal_i2c.h"

#if CONFIG_BRUCE_BOARD_I2C_ENABLED && !CONFIG_BRUCE_BOARD_I2C_HAL

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static i2c_master_bus_handle_t s_bus;
static bool s_init_attempted;

static void board_i2c__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void board_i2c__unlock(void) { xSemaphoreGive(s_mutex); }

i2c_master_bus_handle_t board_i2c__acquire(void) {
    board_i2c__lock();
    if (!s_init_attempted) {
        s_init_attempted = true;
        i2c_master_bus_config_t bus_config = {
            .i2c_port = -1,
            .sda_io_num = (gpio_num_t)CONFIG_BRUCE_BOARD_I2C_SDA_GPIO,
            .scl_io_num = (gpio_num_t)CONFIG_BRUCE_BOARD_I2C_SCL_GPIO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&bus_config, &s_bus) != ESP_OK) s_bus = NULL;
    }
    i2c_master_bus_handle_t bus = s_bus;
    board_i2c__unlock();
    return bus;
}

bool board_i2c__pins(int *sda, int *scl) {
    *sda = CONFIG_BRUCE_BOARD_I2C_SDA_GPIO;
    *scl = CONFIG_BRUCE_BOARD_I2C_SCL_GPIO;
    return true;
}

uint32_t board_i2c__speed_hz(void) { return CONFIG_BRUCE_BOARD_I2C_FREQ_HZ; }

#else

/* The first I2C bus in board.c, if any. */
static const hal_i2c_master_config_t *board_i2c__hal_config(void) {
    const hal_device_t *bus = hal_device_first(HAL_CLASS_I2C);
    return hal_i2c_bus(bus) != NULL ? bus->config : NULL;
}

i2c_master_bus_handle_t board_i2c__acquire(void) { return hal_i2c_bus(hal_device_first(HAL_CLASS_I2C)); }

bool board_i2c__pins(int *sda, int *scl) {
    const hal_i2c_master_config_t *config = board_i2c__hal_config();
    if (config == NULL) return false;
    *sda = config->sda;
    *scl = config->scl;
    return true;
}

uint32_t board_i2c__speed_hz(void) {
    const hal_i2c_master_config_t *config = board_i2c__hal_config();
    return config != NULL ? config->speed_hz : 100000;
}

#endif
