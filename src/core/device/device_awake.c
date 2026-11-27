#include "core/cellular/cellular.h"
#include "core/device/device.h"
#include "core/gps/gps.h"
#include "core/process/process.h"
#include "core_sdk/audio.h"
#include "core_sdk/device.h"
#include "core_sdk/process.h"
#include "core_sdk/wifi.h"

#include <stdint.h>

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

#define DEVICE__AWAKE_HOLDERS 8

/* Processes holding the device awake; a slot clears when its process ends. */
static struct {
    bruce_process_id_t process;
    bruce_resource_id_t resource;
} s_holders[DEVICE__AWAKE_HOLDERS];
static portMUX_TYPE s_holders_lock = portMUX_INITIALIZER_UNLOCKED;

static int device__holder_slot(bruce_process_id_t process) {
    for (int i = 0; i < DEVICE__AWAKE_HOLDERS; ++i) {
        if (s_holders[i].process == process) return i;
    }
    return -1;
}

static void device__holder_cleanup(void *context) {
    int slot = (int)(intptr_t)context;
    portENTER_CRITICAL(&s_holders_lock);
    s_holders[slot].process = BRUCE_PROCESS_ID_INVALID;
    s_holders[slot].resource = BRUCE_RESOURCE_ID_INVALID;
    portEXIT_CRITICAL(&s_holders_lock);
}

bruce_result_t device__prevent_sleep(bool prevent) {
    bruce_process_id_t self = process__current_id();
    if (self == BRUCE_PROCESS_ID_INVALID) return BRUCE_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_holders_lock);
    int slot = device__holder_slot(self);
    if (!prevent) {
        bruce_resource_id_t resource = slot >= 0 ? s_holders[slot].resource : BRUCE_RESOURCE_ID_INVALID;
        if (slot >= 0) {
            s_holders[slot].process = BRUCE_PROCESS_ID_INVALID;
            s_holders[slot].resource = BRUCE_RESOURCE_ID_INVALID;
        }
        portEXIT_CRITICAL(&s_holders_lock);
        if (resource != BRUCE_RESOURCE_ID_INVALID) (void)process_registry__resource_release(resource);
        return BRUCE_OK;
    }
    if (slot < 0) {
        slot = device__holder_slot(BRUCE_PROCESS_ID_INVALID);
        if (slot >= 0) s_holders[slot].process = self;
    } else {
        slot = -2; /* Already held. */
    }
    portEXIT_CRITICAL(&s_holders_lock);
    if (slot == -2) return BRUCE_OK;
    if (slot < 0) return BRUCE_ERR_RESOURCE_LIMIT;
    bruce_resource_id_t resource = process_registry__resource_register(device__holder_cleanup, (void *)(intptr_t)slot);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        device__holder_cleanup((void *)(intptr_t)slot);
        return BRUCE_ERR_NO_MEMORY;
    }
    s_holders[slot].resource = resource;
    return BRUCE_OK;
}

bool device__app_awake(void) {
    portENTER_CRITICAL(&s_holders_lock);
    bool held = false;
    for (int i = 0; i < DEVICE__AWAKE_HOLDERS; ++i) held |= s_holders[i].process != BRUCE_PROCESS_ID_INVALID;
    portEXIT_CRITICAL(&s_holders_lock);
    return held;
}

const char *device__awake_reason(void) {
    if (cellular__in_call()) return "call";
    if (audio__file_is_playing()) return "audio";
    /* The console would drop; a charger alone sends no USB frames. */
    if (usb_serial_jtag_is_connected()) return "usb";
    return NULL;
}

/* Things that work only with the chip awake, whether the screen is on or not. */
const char *device__chip_reason(void) {
    if (gps__is_on()) return "gps";
    if (cellular__mux_running()) return "mobile data";
    if (wifi__is_ap_running()) return "wifi ap"; /* Clients would wait out every sleep. */
    return NULL;
}
