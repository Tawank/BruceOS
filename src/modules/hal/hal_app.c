#include "hal_app.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/stdio.h"
#include "hal/hal.h"

/* One line per device: what drives it, where it sits, and how probing went. */
static void hal_app__print(const hal_device_t *device, void *context) {
    (void)context;
    char where[24] = "-";
    if (device->bus != NULL && device->address != 0) {
        snprintf(where, sizeof(where), "%s@0x%02x", device->bus->name, device->address);
    } else if (device->bus != NULL) {
        snprintf(where, sizeof(where), "%s", device->bus->name);
    }
    const hal_device_state_t *state = device->state;
    stdio__printf("%-14s %-10s %-14s %s", device->name, device->driver != NULL ? device->driver->name : "-", where,
                  hal__state_name(state->state));
    if (state->state == HAL_STATE_FAILED || state->state == HAL_STATE_ABSENT) stdio__printf(" (%d)", (int)state->error);
    if (state->powered) stdio__printf(", powered");
    if (state->users > 0) stdio__printf(", on for %d", (int)state->users);
    stdio__printf("\n");
    if (state->state == HAL_STATE_READY && device->driver != NULL && device->driver->describe != NULL) {
        char line[96];
        device->driver->describe(device, line, sizeof(line));
        stdio__printf("%14s %s\n", "", line);
    }
}

int hal_app_main(int argc, char **argv) {
    /* hal start|stop <device>: take or drop a user, e.g. to load the imu's firmware. */
    if (argc == 3 && (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "stop") == 0)) {
        const hal_device_t *device = hal_device_find(argv[2]);
        if (device == NULL) {
            stdio__printf("No device %s\n", argv[2]);
            return 1;
        }
        if (strcmp(argv[1], "stop") == 0) {
            hal_device_put(device);
            return 0;
        }
        bruce_result_t result = hal_device_get(device);
        stdio__printf(result == BRUCE_OK ? "%s started\n" : "%s: start failed (%d)\n", argv[2], (int)result);
        return result == BRUCE_OK ? 0 : 1;
    }
    if (argc > 1) {
        stdio__printf("Usage: hal [start|stop <device>]\n");
        return 1;
    }
    stdio__printf("Board: %s\n%-14s %-10s %-14s %s\n", hal_board.name, "DEVICE", "DRIVER", "BUS", "STATE");
    hal__for_each(hal_app__print, NULL);
    return 0;
}
