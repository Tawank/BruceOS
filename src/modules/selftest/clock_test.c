#include "clock_test.h"

#include "core_sdk/clock.h"
#include "core_sdk/config.h"

#include <stdint.h>
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

    int64_t scratch_epoch;
    if (clock__datetime_to_epoch(NULL, &scratch_epoch) != BRUCE_ERR_INVALID_ARGUMENT ||
        clock__datetime_to_epoch(&local, NULL) != BRUCE_ERR_INVALID_ARGUMENT ||
        clock__epoch_to_datetime(0, NULL) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] clock: NULL argument accepted by epoch<->datetime conversion\n");
        return false;
    }

    /* 1970-01-01 00:00:00 UTC, a known reference point. */
    bruce_clock_datetime_t epoch0_dt = {.year = 1970, .month = 1, .day = 1};
    int64_t epoch0;
    if (clock__datetime_to_epoch(&epoch0_dt, &epoch0) != BRUCE_OK || epoch0 != 0) {
        printf("[selftest] clock: datetime_to_epoch(1970-01-01) mismatch (%lld)\n", (long long)epoch0);
        return false;
    }
    bruce_clock_datetime_t roundtrip;
    if (clock__epoch_to_datetime(epoch0, &roundtrip) != BRUCE_OK || roundtrip.year != 1970 ||
        roundtrip.month != 1 || roundtrip.day != 1 || roundtrip.hour != 0 || roundtrip.minute != 0 ||
        roundtrip.second != 0) {
        printf("[selftest] clock: epoch_to_datetime(0) mismatch\n");
        return false;
    }

    /* A known Y2K reference point, well clear of the epoch edge case above. */
    bruce_clock_datetime_t y2k_dt = {.year = 2000, .month = 1, .day = 1};
    int64_t y2k_epoch;
    if (clock__datetime_to_epoch(&y2k_dt, &y2k_epoch) != BRUCE_OK || y2k_epoch != 946684800) {
        printf("[selftest] clock: datetime_to_epoch(y2k) mismatch (%lld)\n", (long long)y2k_epoch);
        return false;
    }

    /* clock__get_local_offset_seconds() must match Config's own settings. */
    int64_t expected_offset =
        (int64_t)(config__get_time_timezone() * 3600.0f) + (config__get_time_dst() ? 3600 : 0);
    if (clock__get_local_offset_seconds() != expected_offset) {
        printf("[selftest] clock: get_local_offset_seconds mismatch\n");
        return false;
    }

    printf("[selftest] clock: OK\n");
    return true;
}
