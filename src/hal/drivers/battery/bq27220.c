#include "bq27220.h"

#include "hal/hal_battery.h"
#include "hal/hal_i2c.h"

/* Standard commands, TI's BQ27220 Technical Reference Manual (SLUUBD4A). */
#define BQ27220_BATTERY_STATUS 0x0A
#define BQ27220_STATE_OF_CHARGE 0x2C
/* BatteryStatus bit 0 (DSG) is set while discharging; clear means external
 * power, charging or full (LilyGO's BQ27220 getIsCharging()). */
#define BQ27220_DSG 0x01
#define BQ27220_SPEED_HZ 400000
#define BQ27220_TIMEOUT_MS 100

/* The gauge isn't read here: one that doesn't answer yet is read again later. */
static bruce_result_t bq27220__probe(const hal_device_t *device) {
    i2c_master_dev_handle_t i2c = NULL;
    bruce_result_t result = hal_i2c_open(device, BQ27220_SPEED_HZ, &i2c);
    if (result == BRUCE_OK) device->state->data = i2c;
    return result;
}

static bool bq27220__read_word(i2c_master_dev_handle_t i2c, uint8_t command, uint16_t *out) {
    uint8_t raw[2];
    if (i2c_master_transmit_receive(i2c, &command, 1, raw, sizeof(raw), BQ27220_TIMEOUT_MS) != ESP_OK) return false;
    *out = (uint16_t)(raw[0] | raw[1] << 8);
    return true;
}

static bruce_result_t bq27220__read(const hal_device_t *device, hal_battery_reading_t *out) {
    i2c_master_dev_handle_t i2c = device->state->data;
    uint16_t percent = 0;
    uint16_t status = 0;
    if (!bq27220__read_word(i2c, BQ27220_STATE_OF_CHARGE, &percent) ||
        !bq27220__read_word(i2c, BQ27220_BATTERY_STATUS, &status)) {
        return BRUCE_ERR_IO;
    }
    out->percent = percent > 100 ? 100 : percent;
    out->charging = (status & BQ27220_DSG) == 0;
    return BRUCE_OK;
}

static const hal_battery_api_t bq27220__api = {.read = bq27220__read};

const hal_driver_t bq27220_driver = {
    .name = "bq27220",
    .class_id = HAL_CLASS_BATTERY,
    .probe = bq27220__probe,
    .api = &bq27220__api,
};
