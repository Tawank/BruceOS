#include "gps_test.h"

#include <math.h>
#include <stdio.h>

#include "core/gps/gps.h"

static bool selftest__near(double a, double b) { return fabs(a - b) < 1e-6; }

bool selftest__run_gps_parse_case(void) {
    gps_parser_t parser = {0};
    bruce_gps_fix_t *fixp = &parser.fix;
    bool position = false;
    bool rmc = gps__parse_sentence(
        "$GNRMC,123519.00,A,5003.6000,N,01956.4000,E,10.0,84.4,230394,,,A*44\r\n", &parser, &position
    );
    bool rmc_ok = rmc && position && fixp->fix && selftest__near(fixp->latitude, 50.06) &&
                  selftest__near(fixp->longitude, 19.94) && fabsf(fixp->speed_kmh - 18.52f) < 0.01f &&
                  fixp->time_valid && fixp->utc.year == 2094 && fixp->utc.month == 3 && fixp->utc.day == 23 &&
                  fixp->utc.hour == 12 && fixp->utc.minute == 35 && fixp->utc.second == 19;

    bool gga = gps__parse_sentence(
        "$GNGGA,123520.00,5003.6000,N,01956.4000,E,1,08,0.9,545.4,M,46.9,M,,*70", &parser, &position
    );
    bool gga_ok = gga && position && fixp->satellites == 8 && fabsf(fixp->hdop - 0.9f) < 0.01f &&
                  fabsf(fixp->altitude_m - 545.4f) < 0.01f;

    bool south = gps__parse_sentence(
        "$GPRMC,000000.00,A,3352.1280,S,15112.5920,W,0.0,,010125,,,A*7C", &parser, &position
    );
    bool south_ok =
        south && selftest__near(fixp->latitude, -33.8688) && selftest__near(fixp->longitude, -151.209867);

    bool lost = gps__parse_sentence("$GPRMC,,V,,,,,,,,,,N*53", &parser, &position) && !position && !fixp->fix &&
                gps__parse_sentence("$GNGGA,,,,,,0,03,,,,,,,*7B", &parser, &position) && fixp->satellites == 3;

    bool rejected = !gps__parse_sentence("$GPRMC,,V,,,,,,,,,,N*54", &parser, &position) &&
                    !gps__parse_sentence("$GPGLL,,,,,,V,N*64", &parser, &position) &&
                    !gps__parse_sentence("GPRMC,,V,,,,,,,,,,N*53", &parser, &position);

    bool in_view = gps__parse_sentence("$GPGSV,3,1,11,10,63,137,17,07,61,098,15,05,59,290,20,08,54,157,30*70",
                                       &parser, &position) &&
                   gps__parse_sentence("$GLGSV,1,1,03,65,64,012,,66,30,280,,81,10,120,*56", &parser, &position) &&
                   gps__parse_sentence("$GPGSV,3,2,11,02,39,223,19,13,28,070,17,26,23,252,,04,14,186,14*79",
                                       &parser, &position) &&
                   fixp->satellites_in_view == 14;
    bool ok = rmc_ok && gga_ok && south_ok && lost && rejected && in_view;
    printf(
        "[selftest] gps/parse: %s (rmc=%d gga=%d south=%d lost=%d rejected=%d in_view=%d)\n", ok ? "OK" : "FAIL",
        rmc_ok, gga_ok, south_ok, lost, rejected, in_view
    );
    return ok;
}
