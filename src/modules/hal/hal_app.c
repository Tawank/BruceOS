#include "hal_app.h"

#include <stdio.h>

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
}

int hal_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    stdio__printf("Board: %s\n%-14s %-10s %-14s %s\n", hal_board.name, "DEVICE", "DRIVER", "BUS", "STATE");
    hal__for_each(hal_app__print, NULL);
    return 0;
}
