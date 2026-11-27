#pragma once

#include "core_sdk/device.h"
#include "hal.h"

/* Operations of a HAL_CLASS_CHARGER driver: the chip between the USB port
 * and the battery. Fields it can't tell are left as device__get_power() says. */
typedef struct {
    bruce_result_t (*read)(const hal_device_t *device, bool measure, bruce_device_power_t *out);
    /* Optional: 5 V out of the USB port from the battery, for a USB device. */
    bruce_result_t (*set_otg)(const hal_device_t *device, bool on);
    /* Optional: disconnects the battery (ship mode) until the charger's
     * button or a USB plug brings it back; the board then starts afresh. */
    bruce_result_t (*ship_mode)(const hal_device_t *device);
} hal_charger_api_t;

/* Reads a ready charger device: the status at once, the voltage and current
 * as last measured. `measure` takes in the last measurement and starts the
 * next, where measuring is slow or costs power. */
bruce_result_t hal_charger_read(const hal_device_t *device, bool measure, bruce_device_power_t *out);
/* BRUCE_ERR_UNSUPPORTED when the charger can't power the port. */
bruce_result_t hal_charger_set_otg(const hal_device_t *device, bool on);
/* BRUCE_ERR_UNSUPPORTED when the charger can't disconnect the battery. */
bruce_result_t hal_charger_ship_mode(const hal_device_t *device);
