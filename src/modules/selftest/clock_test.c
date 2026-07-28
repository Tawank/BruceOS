#include "clock_test.h"

#include "core_sdk/clock.h"

#include <stdio.h>

bool selftest__run_clock_case(void) {
    if (clock__get_utc(NULL) != BRUCE_ERR_INVALID_ARGUMENT ||
        clock__get_local(NULL) != BRUCE_ERR_INVALID_ARGUMENT || clock__set_local(NULL) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] clock: NULL argument accepted\n");
        return false;
    }
    bruce_clock_datetime_t invalid = {.year = 2026, .month = 2, .day = 29, .hour = 12};
    if (clock__set_local(&invalid) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] clock: invalid calendar date accepted\n");
        return false;
    }
    bruce_clock_datetime_t local;
    bruce_result_t result = clock__get_local(&local);
    if (result != BRUCE_OK && result != BRUCE_ERR_INVALID_STATE) {
        printf("[selftest] clock: local read failed (%d)\n", result);
        return false;
    }
    if (clock__get_ntp_server() == NULL || clock__get_ntp_server()[0] == '\0') return false;
    printf("[selftest] clock: OK\n");
    return true;
}
