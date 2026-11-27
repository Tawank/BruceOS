#include "sy6970.h"

#include <stdio.h>

#include "hal/hal_charger.h"
#include "hal/hal_i2c.h"

/* Registers from Silergy's AN_SY6970 (Rev 0.1). */
#define SY6970_REG_ADC 0x02    /* Bit 7 CONV_START: one measurement, done within 1 s. */
#define SY6970_REG_STATUS 0x0B /* 7:5 BUS_STAT, 4:3 CHRG_STAT, 2 PG_STAT. */
#define SY6970_REG_VBUS 0x11   /* 6:0 BUSV: 2.6 V + 100 mV steps. */
#define SY6970_REG_ICHG 0x12   /* 6:0 ICHGR: 50 mA steps. */
#define SY6970_REG_PART 0x14   /* 5:3 PN, 001. */
#define SY6970_CONV_START 0x80
#define SY6970_PN 1
#define SY6970_SPEED_HZ 400000
#define SY6970_TIMEOUT_MS 100

typedef struct {
    i2c_master_dev_handle_t i2c;
    bool measuring; /* A measurement was started while something was plugged in. */
    int32_t input_mv;
    int32_t charge_ma;
} sy6970_t;

static sy6970_t s_chip;

static bool sy6970__read(i2c_master_dev_handle_t i2c, uint8_t reg, uint8_t *out) {
    return i2c_master_transmit_receive(i2c, &reg, 1, out, 1, SY6970_TIMEOUT_MS) == ESP_OK;
}

static bool sy6970__write(i2c_master_dev_handle_t i2c, uint8_t reg, uint8_t value) {
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(i2c, data, sizeof(data), SY6970_TIMEOUT_MS) == ESP_OK;
}

static bruce_result_t sy6970__probe(const hal_device_t *device) {
    i2c_master_dev_handle_t i2c = NULL;
    bruce_result_t result = hal_i2c_open(device, SY6970_SPEED_HZ, &i2c);
    if (result != BRUCE_OK) return result;
    uint8_t part = 0;
    if (!sy6970__read(i2c, SY6970_REG_PART, &part) || (part >> 3 & 0x07) != SY6970_PN) return BRUCE_ERR_NOT_FOUND;
    s_chip = (sy6970_t){.i2c = i2c, .input_mv = -1, .charge_ma = -1};
    device->state->data = &s_chip;
    return BRUCE_OK;
}

static const bruce_power_input_t s_inputs[8] = {
    BRUCE_POWER_INPUT_NONE,         BRUCE_POWER_INPUT_USB_HOST,     BRUCE_POWER_INPUT_USB_CHARGING_HOST,
    BRUCE_POWER_INPUT_USB_CHARGER,  BRUCE_POWER_INPUT_FAST_CHARGER, BRUCE_POWER_INPUT_ADAPTER,
    BRUCE_POWER_INPUT_ADAPTER,      BRUCE_POWER_INPUT_OTG,
};

static const bruce_charge_state_t s_charges[4] = {
    BRUCE_CHARGE_NOT_CHARGING, BRUCE_CHARGE_PRECHARGE, BRUCE_CHARGE_CHARGING, BRUCE_CHARGE_FULL,
};

/* With something plugged in, `measure` takes in the last measurement and
 * starts the next (within 1 s); nothing is written while on battery. */
static bruce_result_t sy6970__read_power(const hal_device_t *device, bool measure, bruce_device_power_t *out) {
    sy6970_t *chip = device->state->data;
    uint8_t status = 0;
    if (!sy6970__read(chip->i2c, SY6970_REG_STATUS, &status)) return BRUCE_ERR_IO;
    out->input = s_inputs[status >> 5 & 0x07];
    out->charge = s_charges[status >> 3 & 0x03];
    out->power_good = (status & 0x04) != 0;
    if (out->input == BRUCE_POWER_INPUT_NONE) {
        chip->measuring = false;
        chip->input_mv = chip->charge_ma = -1;
    } else if (measure) {
        uint8_t vbus = 0, ichg = 0, adc = 0;
        if (chip->measuring && sy6970__read(chip->i2c, SY6970_REG_VBUS, &vbus) &&
            sy6970__read(chip->i2c, SY6970_REG_ICHG, &ichg)) {
            chip->input_mv = 2600 + (vbus & 0x7F) * 100;
            chip->charge_ma = (ichg & 0x7F) * 50;
        }
        chip->measuring = sy6970__read(chip->i2c, SY6970_REG_ADC, &adc) &&
                          sy6970__write(chip->i2c, SY6970_REG_ADC, adc | SY6970_CONV_START);
    }
    out->input_mv = chip->input_mv;
    out->charge_ma = chip->charge_ma;
    return BRUCE_OK;
}

static void sy6970__describe(const hal_device_t *device, char *out, size_t size) {
    static const char *const inputs[] = {"unknown", "no input", "USB host", "USB charging host", "USB charger",
                                         "fast charger", "adapter", "OTG"};
    static const char *const charges[] = {"unknown", "not charging", "pre-charge", "charging", "full"};
    bruce_device_power_t power;
    if (sy6970__read_power(device, false, &power) != BRUCE_OK) {
        snprintf(out, size, "no answer");
        return;
    }
    snprintf(out, size, "%s%s, %s", inputs[power.input], power.power_good ? "" : " (not power good)",
             charges[power.charge]);
}

static const hal_charger_api_t sy6970__api = {.read = sy6970__read_power};

const hal_driver_t sy6970_driver = {
    .name = "sy6970",
    .class_id = HAL_CLASS_CHARGER,
    .probe = sy6970__probe,
    .describe = sy6970__describe,
    .api = &sy6970__api,
};
