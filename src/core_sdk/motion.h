#pragma once

#include <stdbool.h>

#include "core_sdk/result.h"

/**
 * @brief Motion sensor: which way the device turns.
 *
 * The board's IMU is started by the first motion__read() and turned off
 * again once nobody has read it for a while, so apps simply poll
 * motion__read() while they want orientation.
 */

/** Orientation from the motion sensor. */
typedef struct {
    bool ready;    /**< The sensor runs and yaw_deg is current. */
    float yaw_deg; /**< Turn about the vertical, counterclockwise seen from above, -180..180. */
} bruce_motion_t;

/** @brief Returns whether the board has a motion sensor. Not permission-gated. */
bool motion__is_available(void);

/**
 * @brief Reads the latest orientation, starting the sensor if needed.
 *
 * yaw_deg is relative: it starts anywhere and drifts slowly, since there is
 * no magnetometer to find north, so use changes in it, not its value. Returns
 * BRUCE_OK with ready false while the sensor starts; the first start after
 * boot loads its firmware and takes a few seconds. The sensor turns off
 * 5 seconds after the last call.
 *
 * @param out Receives the latest orientation.
 * @permission gps
 */
bruce_result_t motion__read(bruce_motion_t *out);
