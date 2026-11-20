#include "gps_app.h"

#include <string.h>

#include "core_sdk/gps.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

static void gps_app__print(const bruce_gps_fix_t *fix) {
    if (!fix->receiving) {
        stdio__printf("No data from the receiver\n");
        return;
    }
    if (fix->fix) {
        stdio__printf(
            "%.6f, %.6f  alt %.0f m  %.1f km/h  sats %u  hdop %.1f", fix->latitude, fix->longitude,
            (double)fix->altitude_m, (double)fix->speed_kmh, fix->satellites, (double)fix->hdop
        );
    } else {
        stdio__printf("Searching, sats %u used, %u in view", fix->satellites, fix->satellites_in_view);
    }
    if (fix->time_valid) {
        stdio__printf(
            "  %04u-%02u-%02u %02u:%02u:%02u UTC", fix->utc.year, fix->utc.month, fix->utc.day, fix->utc.hour,
            fix->utc.minute, fix->utc.second
        );
    }
    stdio__printf("\n");
}

int gps_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        stdio__printf("Usage: gps\nPrints the GPS position every second until interrupted.\n");
        return BRUCE_OK;
    }
    if (!gps__is_available()) {
        stdio__printf("This board has no GPS receiver\n");
        return BRUCE_ERR_UNSUPPORTED;
    }
    while (process__current_signal() == 0) {
        bruce_gps_fix_t fix;
        bruce_result_t result = gps__read(&fix);
        if (result != BRUCE_OK) return result;
        gps_app__print(&fix);
        if (runtime__sleep(1000) != BRUCE_OK) break;
    }
    return BRUCE_OK;
}
