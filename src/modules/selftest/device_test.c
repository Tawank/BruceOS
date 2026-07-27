#include "device_test.h"

#include <stdio.h>

#include "core_sdk/device.h"

bool selftest__run_device_state_case(void) {
    if (device__get_time(NULL) != BRUCE_ERR_INVALID_ARGUMENT ||
        device__get_date(NULL) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] device/state: NULL output was accepted\n");
        return false;
    }

    bruce_device_time_t time;
    bruce_result_t time_result = device__get_time(&time);
    if (time_result != BRUCE_OK && time_result != BRUCE_ERR_INVALID_STATE) {
        printf("[selftest] device/state: time read failed (%d)\n", time_result);
        return false;
    }
    if (time_result == BRUCE_OK && (time.hour > 23 || time.minute > 59 || time.second > 59)) {
        printf("[selftest] device/state: invalid time fields\n");
        return false;
    }

    bruce_device_date_t date;
    bruce_result_t date_result = device__get_date(&date);
    if (date_result != BRUCE_OK && date_result != BRUCE_ERR_INVALID_STATE) {
        printf("[selftest] device/state: date read failed (%d)\n", date_result);
        return false;
    }
    if (date_result == BRUCE_OK &&
        (date.year < 2020 || date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31)) {
        printf("[selftest] device/state: invalid date fields\n");
        return false;
    }

    int battery = device__get_battery();
    if (battery < 0 || battery > 100) {
        printf("[selftest] device/state: battery read failed (%d)\n", battery);
        return false;
    }
    printf("[selftest] device/state: OK\n");
    return true;
}
