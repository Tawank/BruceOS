#include "sx1262.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_gpio.h"
#include "hal/hal_rf.h"
#include "hal/hal_spi.h"

/* Commands and values as RadioLib's SX126x driver (which LilyGO's T-Deck
 * MAX firmware uses) sends them. */
#define SX1262_SET_SLEEP 0x84
#define SX1262_SET_STANDBY 0x80
#define SX1262_SET_RX 0x82
#define SX1262_SET_REGULATOR_MODE 0x96
#define SX1262_CALIBRATE 0x89
#define SX1262_CALIBRATE_IMAGE 0x98
#define SX1262_SET_DIO2_AS_RF_SWITCH 0x9D
#define SX1262_SET_DIO3_AS_TCXO 0x97
#define SX1262_SET_RF_FREQUENCY 0x86
#define SX1262_SET_PACKET_TYPE 0x8A
#define SX1262_SET_MODULATION_PARAMS 0x8B
#define SX1262_GET_RSSI_INST 0x15
#define SX1262_READ_REGISTER 0x1D
#define SX1262_REG_VERSION 0x0320
#define SX1262_STANDBY_RC 0x00
#define SX1262_SLEEP_COLD 0x00
#define SX1262_REGULATOR_DC_DC 0x01
#define SX1262_CALIBRATE_ALL 0x7F
#define SX1262_PACKET_GFSK 0x00
#define SX1262_RX_CONTINUOUS 0xFFFFFF
/* GFSK 4.8 kb/s, no shaping, 117.3 kHz receive bandwidth, 5 kHz deviation:
 * bit rate 32 * 32 MHz / 4800, deviation 5000 * 2^25 / 32 MHz. */
#define SX1262_BIT_RATE 213333u
#define SX1262_RX_BW_117_3 0x0B
#define SX1262_DEVIATION 5242u
#define SX1262_SPI_HZ 8000000
#define SX1262_BUSY_MS 100
#define SX1262_RSSI_SETTLE_US 1000
#define SX1262_RSSI_SAMPLES 4
#define SX1262_IMAGE_SPAN_MHZ 4 /* RadioLib calibrates the image around the frequency by this much. */

typedef struct {
    spi_device_handle_t spi;
    int busy;
    int16_t image_min_mhz; /* Calibrated image rejection, 0 until done. */
    int16_t image_max_mhz;
    char version[17];
} sx1262_t;

static const hal_rf_band_t sx1262__bands[] = {{150000, 960000}};

/* BUSY is high while the chip works on a command, or wakes up. */
static bool sx1262__wait(const sx1262_t *chip) {
    int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)chip->busy)) {
        int64_t elapsed = esp_timer_get_time() - start;
        if (elapsed > SX1262_BUSY_MS * 1000) return false;
        if (elapsed > 2000) vTaskDelay(1);
        else esp_rom_delay_us(10);
    }
    return true;
}

static bool sx1262__transfer(const sx1262_t *chip, const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!sx1262__wait(chip)) return false;
    spi_transaction_t t = {.length = len * 8, .tx_buffer = tx, .rx_buffer = rx};
    bool ok = spi_device_polling_transmit(chip->spi, &t) == ESP_OK;
    esp_rom_delay_us(1); /* For BUSY to rise, as RadioLib waits. */
    return ok;
}

/* The status the chip returns after the opcode, checked as RadioLib does:
 * a timed out, invalid or failed command, or no chip (0x00, 0xFF). */
static bool sx1262__status_ok(uint8_t status) {
    uint8_t command = status & 0x0E;
    return command != 0x06 && command != 0x08 && command != 0x0A && status != 0x00 && status != 0xFF;
}

static bool sx1262__command(const sx1262_t *chip, uint8_t command, const uint8_t *data, size_t len) {
    uint8_t tx[9] = {command}, rx[9] = {0};
    if (len > sizeof(tx) - 1) return false;
    if (len > 0) memcpy(tx + 1, data, len);
    if (!sx1262__transfer(chip, tx, rx, len + 1)) return false;
    /* RadioLib doesn't check these: the chip is busy changing mode. */
    bool check = len > 0 && command != SX1262_SET_RX && command != SX1262_CALIBRATE && command != SX1262_SET_SLEEP;
    return !check || sx1262__status_ok(rx[1]);
}

static bool sx1262__command1(const sx1262_t *chip, uint8_t command, uint8_t value) {
    return sx1262__command(chip, command, &value, 1);
}

static void sx1262__reset(const hal_device_t *device) {
    const hal_pin_t *reset = hal_device_pin(device, "reset");
    (void)hal_pin_set(reset, true);
    esp_rom_delay_us(1000); /* RadioLib's 1 ms. */
    (void)hal_pin_set(reset, false);
}

/* After reset it refuses the first commands; RadioLib retries standby for up to a second. */
static bool sx1262__standby(const sx1262_t *chip) {
    for (int i = 0; i < 100; ++i) {
        if (sx1262__command1(chip, SX1262_SET_STANDBY, SX1262_STANDBY_RC)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static bool sx1262__read_version(const sx1262_t *chip, char *out) {
    uint8_t tx[4 + 16] = {SX1262_READ_REGISTER, SX1262_REG_VERSION >> 8, SX1262_REG_VERSION & 0xFF};
    uint8_t rx[sizeof(tx)] = {0};
    if (!sx1262__transfer(chip, tx, rx, sizeof(tx))) return false;
    memcpy(out, rx + 4, 16);
    out[16] = '\0';
    return strncmp(out, "SX126", 5) == 0;
}

static void sx1262__power_off(const hal_device_t *device, const sx1262_t *chip) {
    (void)sx1262__command1(chip, SX1262_SET_SLEEP, SX1262_SLEEP_COLD);
    esp_rom_delay_us(1000);
    (void)hal_device_power(device, false);
}

static bruce_result_t sx1262__probe(const hal_device_t *device) {
    int cs = hal_pin_soc_gpio(hal_device_pin(device, "cs"));
    int busy = hal_pin_soc_gpio(hal_device_pin(device, "busy"));
    if (cs < 0 || busy < 0 || hal_device_pin(device, "reset") == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    sx1262_t *chip = calloc(1, sizeof(*chip));
    if (chip == NULL) return BRUCE_ERR_NO_MEMORY;
    chip->busy = busy;
    (void)gpio_set_direction((gpio_num_t)busy, GPIO_MODE_INPUT);
    spi_device_interface_config_t config = {
        .clock_speed_hz = SX1262_SPI_HZ, .mode = 0, .spics_io_num = cs, .queue_size = 1
    };
    bruce_result_t result = hal_spi_open(device, &config, &chip->spi);
    if (result != BRUCE_OK) {
        free(chip);
        return result;
    }
    result = hal_device_power(device, true);
    if (result == BRUCE_OK) {
        sx1262__reset(device);
        if (!sx1262__standby(chip) || !sx1262__read_version(chip, chip->version)) result = BRUCE_ERR_NOT_FOUND;
        sx1262__power_off(device, chip);
    }
    if (result != BRUCE_OK) {
        spi_bus_remove_device(chip->spi);
        free(chip);
        return result;
    }
    device->state->data = chip;
    return BRUCE_OK;
}

/* Powered fresh each time: TCXO, calibration, regulator and a GFSK receiver to measure with. */
static bruce_result_t sx1262__start(const hal_device_t *device) {
    sx1262_t *chip = device->state->data;
    const sx1262_config_t *config = device->config;
    bruce_result_t result = hal_device_power(device, true);
    if (result != BRUCE_OK) return result;
    sx1262__reset(device);
    chip->image_min_mhz = chip->image_max_mhz = 0;
    bool ok = sx1262__standby(chip);
    if (ok && config != NULL && config->tcxo_code != 0) {
        uint32_t delay = (uint32_t)(config->tcxo_delay_us * 64u / 1000u); /* In 15.625 us steps. */
        uint8_t tcxo[] = {config->tcxo_code, (uint8_t)(delay >> 16), (uint8_t)(delay >> 8), (uint8_t)delay};
        ok = sx1262__command(chip, SX1262_SET_DIO3_AS_TCXO, tcxo, sizeof(tcxo));
    }
    ok = ok && sx1262__command1(chip, SX1262_SET_PACKET_TYPE, SX1262_PACKET_GFSK) &&
         sx1262__command1(chip, SX1262_CALIBRATE, SX1262_CALIBRATE_ALL);
    esp_rom_delay_us(5000); /* RadioLib's wait before calibration ends. */
    ok = ok && sx1262__wait(chip) && sx1262__command1(chip, SX1262_SET_REGULATOR_MODE, SX1262_REGULATOR_DC_DC);
    if (ok && config != NULL && config->dio2_rf_switch) ok = sx1262__command1(chip, SX1262_SET_DIO2_AS_RF_SWITCH, 1);
    const uint8_t modulation[] = {
        (uint8_t)(SX1262_BIT_RATE >> 16), (uint8_t)(SX1262_BIT_RATE >> 8), (uint8_t)SX1262_BIT_RATE, 0x00,
        SX1262_RX_BW_117_3, (uint8_t)(SX1262_DEVIATION >> 16), (uint8_t)(SX1262_DEVIATION >> 8),
        (uint8_t)SX1262_DEVIATION
    };
    ok = ok && sx1262__command(chip, SX1262_SET_MODULATION_PARAMS, modulation, sizeof(modulation));
    if (!ok) {
        sx1262__power_off(device, chip);
        return BRUCE_ERR_IO;
    }
    return BRUCE_OK;
}

static void sx1262__stop(const hal_device_t *device) {
    sx1262_t *chip = device->state->data;
    (void)sx1262__standby(chip);
    sx1262__power_off(device, chip);
}

static size_t sx1262__get_bands(const hal_device_t *device, const hal_rf_band_t **out_bands) {
    (void)device;
    *out_bands = sx1262__bands;
    return sizeof(sx1262__bands) / sizeof(sx1262__bands[0]);
}

static bruce_result_t sx1262__rssi(const hal_device_t *device, uint32_t frequency_khz, int16_t *out_dbm) {
    sx1262_t *chip = device->state->data;
    if (!sx1262__command1(chip, SX1262_SET_STANDBY, SX1262_STANDBY_RC)) return BRUCE_ERR_IO;
    int mhz = (int)(frequency_khz / 1000u);
    if (mhz < chip->image_min_mhz || mhz > chip->image_max_mhz) {
        int low = mhz - SX1262_IMAGE_SPAN_MHZ, high = mhz + SX1262_IMAGE_SPAN_MHZ;
        /* RadioLib: floor((min - 1) / 4) and ceil((max + 1) / 4), in MHz. */
        uint8_t image[] = {(uint8_t)((low - 1) / 4), (uint8_t)((high + 1 + 3) / 4)};
        if (!sx1262__command(chip, SX1262_CALIBRATE_IMAGE, image, sizeof(image)) || !sx1262__wait(chip)) {
            return BRUCE_ERR_IO;
        }
        chip->image_min_mhz = (int16_t)low;
        chip->image_max_mhz = (int16_t)high;
    }
    uint32_t frf = (uint32_t)(((uint64_t)frequency_khz << 25) / 32000u);
    uint8_t frequency[] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    uint8_t rx_timeout[] = {
        (uint8_t)(SX1262_RX_CONTINUOUS >> 16), (uint8_t)(SX1262_RX_CONTINUOUS >> 8), (uint8_t)SX1262_RX_CONTINUOUS
    };
    if (!sx1262__command(chip, SX1262_SET_RF_FREQUENCY, frequency, sizeof(frequency)) ||
        !sx1262__command(chip, SX1262_SET_RX, rx_timeout, sizeof(rx_timeout))) {
        return BRUCE_ERR_IO;
    }
    esp_rom_delay_us(SX1262_RSSI_SETTLE_US);
    /* The strongest of a few readings: remote controls send in short bursts. */
    int best = 255;
    for (int i = 0; i < SX1262_RSSI_SAMPLES; ++i) {
        uint8_t tx[3] = {SX1262_GET_RSSI_INST}, rx[3] = {0};
        if (!sx1262__transfer(chip, tx, rx, sizeof(tx))) return BRUCE_ERR_IO;
        if (rx[2] < best) best = rx[2];
        esp_rom_delay_us(250);
    }
    *out_dbm = (int16_t)(-best / 2);
    return BRUCE_OK;
}

static void sx1262__describe(const hal_device_t *device, char *out, size_t size) {
    const sx1262_t *chip = device->state->data;
    snprintf(out, size, "%s", chip->version);
}

static const hal_rf_api_t sx1262__api = {.bands = sx1262__get_bands, .rssi = sx1262__rssi};

const hal_driver_t sx1262_driver = {
    .name = "sx1262",
    .class_id = HAL_CLASS_RF,
    .probe = sx1262__probe,
    .start = sx1262__start,
    .stop = sx1262__stop,
    .describe = sx1262__describe,
    .api = &sx1262__api,
};
