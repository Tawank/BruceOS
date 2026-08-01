#include "device_test.h"

#include <stdio.h>

#include "core_sdk/clock.h"
#include "core_sdk/device.h"

bool selftest__run_device_state_case(void) {
    if (clock__get_local(NULL) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] device/state: NULL output was accepted\n");
        return false;
    }

    bruce_clock_datetime_t local;
    bruce_result_t clock_result = clock__get_local(&local);
    if (clock_result != BRUCE_OK && clock_result != BRUCE_ERR_INVALID_STATE) {
        printf("[selftest] device/state: clock read failed (%d)\n", clock_result);
        return false;
    }
    if (clock_result == BRUCE_OK && (local.hour > 23 || local.minute > 59 || local.second > 59)) {
        printf("[selftest] device/state: invalid time fields\n");
        return false;
    }

    if (clock_result == BRUCE_OK &&
        (local.year < 2020 || local.month < 1 || local.month > 12 || local.day < 1 || local.day > 31)) {
        printf("[selftest] device/state: invalid date fields\n");
        return false;
    }

    int battery = device__get_battery();
    if ((battery < 0 && battery != BRUCE_ERR_UNSUPPORTED) || battery > 100) {
        printf("[selftest] device/state: battery read failed (%d)\n", battery);
        return false;
    }
    printf("[selftest] device/state: OK\n");
    return true;
}
