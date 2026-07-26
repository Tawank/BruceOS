#include "nrf24.h"

#include "core_sdk/nrf24.h"
#include "core_sdk/permission.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define NRF24_SPI_HOST SPI3_HOST
#define NRF24_SPI_CLOCK_HZ 10000000

#define NRF24_CMD_R_REGISTER 0x00u
#define NRF24_CMD_W_REGISTER 0x20u
#define NRF24_CMD_NOP 0xffu

#define NRF24_REG_CONFIG 0x00u
#define NRF24_REG_EN_AA 0x01u
#define NRF24_REG_EN_RXADDR 0x02u
#define NRF24_REG_SETUP_AW 0x03u
#define NRF24_REG_RF_CH 0x05u
#define NRF24_REG_RF_SETUP 0x06u
#define NRF24_REG_STATUS 0x07u
#define NRF24_REG_RPD 0x09u
#define NRF24_REG_RX_ADDR_P0 0x0au

#define NRF24_CONFIG_PWR_UP (1u << 1)
#define NRF24_CONFIG_PRIM_RX (1u << 0)

static SemaphoreHandle_t s_nrf24_mutex;
static spi_device_handle_t s_nrf24_device;
static uint8_t s_nrf24_channel = BRUCE_NRF24_DEFAULT_CHANNEL;

static bruce_result_t nrf24__esp_result(esp_err_t error)
{
    if (error == ESP_OK) return BRUCE_OK;
    if (error == ESP_ERR_INVALID_ARG) return BRUCE_ERR_INVALID_ARGUMENT;
    if (error == ESP_ERR_NO_MEM) return BRUCE_ERR_NO_MEMORY;
    if (error == ESP_ERR_INVALID_STATE || error == ESP_ERR_NOT_FOUND) return BRUCE_ERR_BUSY;
    if (error == ESP_ERR_TIMEOUT) return BRUCE_ERR_TIMEOUT;
    if (error == ESP_ERR_NOT_SUPPORTED) return BRUCE_ERR_UNSUPPORTED;
    return BRUCE_ERR_IO;
}

void nrf24__get_pins(bruce_nrf24_pins_t *out_pins)
{
    if (out_pins == NULL) return;
    *out_pins = (bruce_nrf24_pins_t){
        .sck = CONFIG_BRUCE_NRF24_SCK_GPIO,
        .miso = CONFIG_BRUCE_NRF24_MISO_GPIO,
        .mosi = CONFIG_BRUCE_NRF24_MOSI_GPIO,
        .cs = CONFIG_BRUCE_NRF24_CS_GPIO,
        .ce = CONFIG_BRUCE_NRF24_CE_GPIO,
    };
}

bruce_result_t nrf24__init(void)
{
    if (s_nrf24_mutex == NULL) {
        s_nrf24_mutex = xSemaphoreCreateMutex();
        if (s_nrf24_mutex == NULL) return BRUCE_ERR_NO_MEMORY;
    }
    if (s_nrf24_device != NULL) return BRUCE_OK;

    gpio_config_t ce_config = {
        .pin_bit_mask = 1ULL << CONFIG_BRUCE_NRF24_CE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t error = gpio_config(&ce_config);
    if (error != ESP_OK) return nrf24__esp_result(error);
    gpio_set_level(CONFIG_BRUCE_NRF24_CE_GPIO, 0);

    spi_bus_config_t bus_config = {
        .sclk_io_num = CONFIG_BRUCE_NRF24_SCK_GPIO,
        .miso_io_num = CONFIG_BRUCE_NRF24_MISO_GPIO,
        .mosi_io_num = CONFIG_BRUCE_NRF24_MOSI_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 33,
    };
    error = spi_bus_initialize(NRF24_SPI_HOST, &bus_config, SPI_DMA_DISABLED);
    if (error != ESP_OK) return nrf24__esp_result(error);

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = NRF24_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_BRUCE_NRF24_CS_GPIO,
        .queue_size = 1,
    };
    error = spi_bus_add_device(NRF24_SPI_HOST, &device_config, &s_nrf24_device);
    if (error != ESP_OK) {
        (void)spi_bus_free(NRF24_SPI_HOST);
        return nrf24__esp_result(error);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    return BRUCE_OK;
}

static bruce_result_t nrf24__transfer(uint8_t command, const uint8_t *write_data,
                                      uint8_t *read_data, size_t size, uint8_t *out_status)
{
    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};
    if (size > 32) return BRUCE_ERR_INVALID_ARGUMENT;
    tx[0] = command;
    if (write_data != NULL && size > 0) memcpy(&tx[1], write_data, size);
    else if (size > 0) memset(&tx[1], NRF24_CMD_NOP, size);
    spi_transaction_t transaction = {
        .length = (size + 1u) * 8u,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    bruce_result_t result = nrf24__esp_result(spi_device_transmit(s_nrf24_device, &transaction));
    if (result != BRUCE_OK) return result;
    if (out_status != NULL) *out_status = rx[0];
    if (read_data != NULL && size > 0) memcpy(read_data, &rx[1], size);
    return BRUCE_OK;
}

static bruce_result_t nrf24__read_register(uint8_t reg, uint8_t *value)
{
    return nrf24__transfer(NRF24_CMD_R_REGISTER | (reg & 0x1fu), NULL, value, 1, NULL);
}

static bruce_result_t nrf24__write_register(uint8_t reg, uint8_t value)
{
    return nrf24__transfer(NRF24_CMD_W_REGISTER | (reg & 0x1fu), &value, NULL, 1, NULL);
}

static bruce_result_t nrf24__check_chip(bool *out_connected)
{
    uint8_t original = 0;
    bruce_result_t result = nrf24__read_register(NRF24_REG_SETUP_AW, &original);
    if (result != BRUCE_OK) return result;
    uint8_t test = original == 1u ? 3u : 1u;
    result = nrf24__write_register(NRF24_REG_SETUP_AW, test);
    uint8_t readback = 0;
    if (result == BRUCE_OK) result = nrf24__read_register(NRF24_REG_SETUP_AW, &readback);
    if (result == BRUCE_OK) (void)nrf24__write_register(NRF24_REG_SETUP_AW, original);
    if (result != BRUCE_OK) return result;
    *out_connected = readback == test;
    return BRUCE_OK;
}

static bruce_result_t nrf24__lock_and_probe(bool *out_connected)
{
    bruce_result_t result = nrf24__init();
    if (result != BRUCE_OK) return result;
    if (xSemaphoreTake(s_nrf24_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return BRUCE_ERR_BUSY;
    result = nrf24__check_chip(out_connected);
    if (result != BRUCE_OK || !*out_connected) {
        xSemaphoreGive(s_nrf24_mutex);
        return result != BRUCE_OK ? result : BRUCE_ERR_NOT_FOUND;
    }
    return BRUCE_OK;
}

bruce_result_t nrf24__probe(bool *out_connected)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_RF);
    if (permission != BRUCE_OK) return permission;
    if (out_connected == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_connected = false;
    bruce_result_t result = nrf24__init();
    if (result != BRUCE_OK) return result;
    if (xSemaphoreTake(s_nrf24_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return BRUCE_ERR_BUSY;
    result = nrf24__check_chip(out_connected);
    xSemaphoreGive(s_nrf24_mutex);
    return result;
}

bruce_result_t nrf24__set_channel(uint8_t channel)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_RF);
    if (permission != BRUCE_OK) return permission;
    if (channel > BRUCE_NRF24_CHANNEL_MAX) return BRUCE_ERR_INVALID_ARGUMENT;
    bool connected = false;
    bruce_result_t result = nrf24__lock_and_probe(&connected);
    if (result == BRUCE_OK) result = nrf24__write_register(NRF24_REG_RF_CH, channel);
    if (result == BRUCE_OK) s_nrf24_channel = channel;
    if (connected) xSemaphoreGive(s_nrf24_mutex);
    return result;
}

bruce_result_t nrf24__get_channel(uint8_t *out_channel)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_RF);
    if (permission != BRUCE_OK) return permission;
    if (out_channel == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bool connected = false;
    bruce_result_t result = nrf24__lock_and_probe(&connected);
    if (result == BRUCE_OK) {
        result = nrf24__read_register(NRF24_REG_RF_CH, out_channel);
        if (result == BRUCE_OK) s_nrf24_channel = *out_channel;
    }
    if (connected) xSemaphoreGive(s_nrf24_mutex);
    return result;
}

bruce_result_t nrf24__scan(uint8_t first_channel, size_t channel_count,
                           uint8_t samples, uint8_t *out_activity)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_RF);
    if (permission != BRUCE_OK) return permission;
    if (channel_count == 0 || out_activity == NULL || samples == 0 ||
        first_channel > BRUCE_NRF24_CHANNEL_MAX ||
        channel_count > (size_t)BRUCE_NRF24_CHANNEL_MAX + 1u - first_channel) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bool connected = false;
    bruce_result_t result = nrf24__lock_and_probe(&connected);
    if (result != BRUCE_OK) return result;

    uint8_t address[5] = {0x55, 0x2a, 0x55, 0x2a, 0x55};
    result = nrf24__write_register(NRF24_REG_CONFIG, NRF24_CONFIG_PWR_UP | NRF24_CONFIG_PRIM_RX);
    if (result == BRUCE_OK) result = nrf24__write_register(NRF24_REG_EN_AA, 0);
    if (result == BRUCE_OK) result = nrf24__write_register(NRF24_REG_EN_RXADDR, 1);
    if (result == BRUCE_OK) result = nrf24__write_register(NRF24_REG_RF_SETUP, 0x06);
    if (result == BRUCE_OK) {
        result = nrf24__transfer(NRF24_CMD_W_REGISTER | NRF24_REG_RX_ADDR_P0, address, NULL, sizeof(address), NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    memset(out_activity, 0, channel_count);
    for (uint8_t sample = 0; result == BRUCE_OK && sample < samples; ++sample) {
        for (size_t index = 0; result == BRUCE_OK && index < channel_count; ++index) {
            uint8_t channel = (uint8_t)(first_channel + index);
            result = nrf24__write_register(NRF24_REG_RF_CH, channel);
            if (result != BRUCE_OK) break;
            gpio_set_level(CONFIG_BRUCE_NRF24_CE_GPIO, 1);
            esp_rom_delay_us(140);
            uint8_t rpd = 0;
            result = nrf24__read_register(NRF24_REG_RPD, &rpd);
            gpio_set_level(CONFIG_BRUCE_NRF24_CE_GPIO, 0);
            if (result == BRUCE_OK && (rpd & 1u) != 0) out_activity[index]++;
        }
    }
    gpio_set_level(CONFIG_BRUCE_NRF24_CE_GPIO, 0);
    (void)nrf24__write_register(NRF24_REG_CONFIG, 0);
    if (nrf24__write_register(NRF24_REG_RF_CH, s_nrf24_channel) != BRUCE_OK && result == BRUCE_OK) {
        result = BRUCE_ERR_IO;
    }
    xSemaphoreGive(s_nrf24_mutex);
    return result;
}
