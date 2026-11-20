#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/clock.h"
#include "core_sdk/result.h"

/**
 * @brief Built-in GPS receiver.
 *
 * The receiver is powered on by the first gps__read() and powered off again
 * once nobody has read it for a while, so apps simply poll gps__read() for as
 * long as they want positions.
 */

typedef struct {
    bool receiving;     /**< NMEA data arrived in the last few seconds. */
    bool fix;           /**< latitude, longitude and altitude_m are valid. */
    double latitude;    /**< Degrees, north positive. */
    double longitude;   /**< Degrees, east positive. */
    float altitude_m;   /**< Above mean sea level. */
    float speed_kmh;
    float course_deg;   /**< Direction of travel, 0 = north. */
    float hdop;         /**< Horizontal dilution of precision; lower is better. */
    uint8_t satellites; /**< Used in the fix. */
    uint8_t satellites_in_view; /**< Seen by the receiver, all constellations. */
    bool time_valid;    /**< utc is valid. */
    bruce_clock_datetime_t utc;
    uint32_t age_ms;    /**< Since the position was last updated. */
} bruce_gps_fix_t;

/** @brief Returns whether the board has a GPS receiver. Not permission-gated. */
bool gps__is_available(void);

/**
 * @brief Reads the latest GPS state, powering the receiver on if needed.
 *
 * Returns BRUCE_OK with fix false while the receiver is starting or still
 * looking for satellites; a cold start can take a minute or more outdoors.
 * The receiver powers off after 30 seconds without a call.
 *
 * @param out Receives the latest state.
 * @permission gps
 */
bruce_result_t gps__read(bruce_gps_fix_t *out);
