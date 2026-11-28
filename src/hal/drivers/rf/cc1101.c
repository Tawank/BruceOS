#include "cc1101.h"

#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_gpio.h"
#include "hal/hal_rf.h"
#include "hal/hal_spi.h"

/* Registers, strobes and the setup as RadioLib's CC1101 driver does it. */
#define CC1101_READ 0x80
#define CC1101_BURST 0x40 /* Also selects a status register on read. */
#define CC1101_SRES 0x30
#define CC1101_SRX 0x34
#define CC1101_SIDLE 0x36
#define CC1101_SPWD 0x39
#define CC1101_IOCFG2 0x00
#define CC1101_IOCFG0 0x02
#define CC1101_FREQ2 0x0D
#define CC1101_MCSM0 0x18
#define CC1101_VERSION 0x31
#define CC1101_RSSI 0x34
#define CC1101_MARCSTATE 0x35
#define CC1101_GDO_HIGH_Z 0x2E
/* Calibrates on every move from idle to receive, power-on timeout as reset (0x04). */
#define CC1101_MCSM0_AUTOCAL 0x14
#define CC1101_MARC_IDLE 0x01
#define CC1101_MARC_RX 0x0D
#define CC1101_RSSI_OFFSET 74
#define CC1101_CRYSTAL_KHZ 26000u
#define CC1101_SPI_HZ 4000000
#define CC1101_RSSI_SETTLE_US 1000
#define CC1101_RSSI_SAMPLES 4

typedef struct {
    spi_device_handle_t spi;
    int cs;
    uint8_t version;
} cc1101_t;

static const hal_rf_band_t cc1101__bands[] = {{300000, 348000}, {387000, 464000}, {779000, 928000}};

/* The driver holds the bus around its own chip select. */
static bool cc1101__transfer(const cc1101_t *chip, const uint8_t *tx, uint8_t *rx, size_t len) {
    if (spi_device_acquire_bus(chip->spi, portMAX_DELAY) != ESP_OK) return false;
    gpio_set_level((gpio_num_t)chip->cs, 0);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = tx, .rx_buffer = rx};
    bool ok = spi_device_polling_transmit(chip->spi, &t) == ESP_OK;
    gpio_set_level((gpio_num_t)chip->cs, 1);
    spi_device_release_bus(chip->spi);
    return ok;
}

static bool cc1101__strobe(const cc1101_t *chip, uint8_t strobe) { return cc1101__transfer(chip, &strobe, NULL, 1); }

static bool cc1101__write(const cc1101_t *chip, uint8_t reg, uint8_t value) {
    uint8_t tx[] = {reg, value};
    return cc1101__transfer(chip, tx, NULL, sizeof(tx));
}

/* Status registers can change while read (errata): read until two agree. */
static bool cc1101__status(const cc1101_t *chip, uint8_t reg, uint8_t *out) {
    uint8_t last = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t tx[2] = {reg | CC1101_READ | CC1101_BURST, 0}, rx[2] = {0};
        if (!cc1101__transfer(chip, tx, rx, sizeof(tx))) return false;
        if (i > 0 && rx[1] == last) break;
        last = rx[1];
    }
    *out = last;
    return true;
}

/* The manual power-on reset, as RadioLib does it: CS low, high, low, then SRES. */
static void cc1101__reset(const cc1101_t *chip) {
    if (spi_device_acquire_bus(chip->spi, portMAX_DELAY) == ESP_OK) {
        gpio_set_level((gpio_num_t)chip->cs, 0);
        esp_rom_delay_us(5);
        gpio_set_level((gpio_num_t)chip->cs, 1);
        esp_rom_delay_us(40);
        gpio_set_level((gpio_num_t)chip->cs, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)chip->cs, 1);
        spi_device_release_bus(chip->spi);
    }
    (void)cc1101__strobe(chip, CC1101_SRES);
}

static bool cc1101__wait_state(const cc1101_t *chip, uint8_t state) {
    for (int i = 0; i < 100; ++i) {
        uint8_t marc = 0;
        if (!cc1101__status(chip, CC1101_MARCSTATE, &marc)) return false;
        if ((marc & 0x1F) == state) return true;
        esp_rom_delay_us(20);
    }
    return false;
}

static bruce_result_t cc1101__probe(const hal_device_t *device) {
    int cs = hal_pin_soc_gpio(hal_device_pin(device, "cs"));
    if (cs < 0) return BRUCE_ERR_INVALID_ARGUMENT;
    cc1101_t *chip = calloc(1, sizeof(*chip));
    if (chip == NULL) return BRUCE_ERR_NO_MEMORY;
    chip->cs = cs;
    gpio_set_level((gpio_num_t)cs, 1);
    (void)gpio_set_direction((gpio_num_t)cs, GPIO_MODE_OUTPUT);
    spi_device_interface_config_t config = {
        .clock_speed_hz = CC1101_SPI_HZ, .mode = 0, .spics_io_num = -1, .queue_size = 1
    };
    bruce_result_t result = hal_spi_open(device, &config, &chip->spi);
    if (result != BRUCE_OK) {
        free(chip);
        return result;
    }
    result = hal_device_power(device, true);
    if (result == BRUCE_OK) {
        cc1101__reset(chip);
        vTaskDelay(pdMS_TO_TICKS(10));
        /* RadioLib accepts 0x14 (current), 0x04 (legacy) and 0x17 (clones). */
        if (!cc1101__status(chip, CC1101_VERSION, &chip->version) ||
            (chip->version != 0x14 && chip->version != 0x04 && chip->version != 0x17)) {
            result = BRUCE_ERR_NOT_FOUND;
        }
        (void)cc1101__strobe(chip, CC1101_SPWD);
        (void)hal_device_power(device, false);
    }
    if (result != BRUCE_OK) {
        spi_bus_remove_device(chip->spi);
        free(chip);
        return result;
    }
    device->state->data = chip;
    return BRUCE_OK;
}

static bruce_result_t cc1101__start(const hal_device_t *device) {
    cc1101_t *chip = device->state->data;
    bruce_result_t result = hal_device_power(device, true);
    if (result != BRUCE_OK) return result;
    cc1101__reset(chip);
    vTaskDelay(pdMS_TO_TICKS(150)); /* RadioLib's wait for the chip after reset. */
    bool ok = cc1101__strobe(chip, CC1101_SIDLE) && cc1101__wait_state(chip, CC1101_MARC_IDLE) &&
              cc1101__write(chip, CC1101_MCSM0, CC1101_MCSM0_AUTOCAL) &&
              cc1101__write(chip, CC1101_IOCFG0, CC1101_GDO_HIGH_Z) && cc1101__write(chip, CC1101_IOCFG2, CC1101_GDO_HIGH_Z);
    if (!ok) {
        (void)hal_device_power(device, false);
        return BRUCE_ERR_IO;
    }
    return BRUCE_OK;
}

static void cc1101__stop(const hal_device_t *device) {
    cc1101_t *chip = device->state->data;
    (void)cc1101__strobe(chip, CC1101_SIDLE);
    (void)cc1101__strobe(chip, CC1101_SPWD);
    (void)hal_device_power(device, false);
}

static size_t cc1101__get_bands(const hal_device_t *device, const hal_rf_band_t **out_bands) {
    (void)device;
    *out_bands = cc1101__bands;
    return sizeof(cc1101__bands) / sizeof(cc1101__bands[0]);
}

static bruce_result_t cc1101__rssi(const hal_device_t *device, uint32_t frequency_khz, int16_t *out_dbm) {
    cc1101_t *chip = device->state->data;
    uint32_t frf = (uint32_t)(((uint64_t)frequency_khz << 16) / CC1101_CRYSTAL_KHZ);
    uint8_t frequency[] = {CC1101_FREQ2 | CC1101_BURST, (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    bool ok = cc1101__strobe(chip, CC1101_SIDLE) && cc1101__wait_state(chip, CC1101_MARC_IDLE) &&
              cc1101__transfer(chip, frequency, NULL, sizeof(frequency)) && cc1101__strobe(chip, CC1101_SRX) &&
              cc1101__wait_state(chip, CC1101_MARC_RX);
    if (!ok) return BRUCE_ERR_IO;
    esp_rom_delay_us(CC1101_RSSI_SETTLE_US);
    int best = -200;
    for (int i = 0; i < CC1101_RSSI_SAMPLES; ++i) {
        uint8_t raw = 0;
        if (!cc1101__status(chip, CC1101_RSSI, &raw)) return BRUCE_ERR_IO;
        /* Two's complement in half dB, minus the offset (RadioLib's 74). */
        int dbm = (int)(int8_t)raw / 2 - CC1101_RSSI_OFFSET;
        if (dbm > best) best = dbm;
        esp_rom_delay_us(250);
    }
    *out_dbm = (int16_t)best;
    return BRUCE_OK;
}

static void cc1101__describe(const hal_device_t *device, char *out, size_t size) {
    const cc1101_t *chip = device->state->data;
    snprintf(out, size, "version 0x%02X", chip->version);
}

static const hal_rf_api_t cc1101__api = {.bands = cc1101__get_bands, .rssi = cc1101__rssi};

const hal_driver_t cc1101_driver = {
    .name = "cc1101",
    .class_id = HAL_CLASS_RF,
    .probe = cc1101__probe,
    .start = cc1101__start,
    .stop = cc1101__stop,
    .describe = cc1101__describe,
    .api = &cc1101__api,
};
