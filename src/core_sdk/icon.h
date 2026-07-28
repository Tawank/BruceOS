#pragma once

#include <stddef.h>

/*
 * Built-in vector icon registry.
 *
 * All icons are authored in a 24x24 viewBox (Material Design Icons style) and
 * are returned as read-only SVG path-data strings.  The returned pointer is
 * valid for the lifetime of the firmware and must not be freed by the caller.
 */

/* ViewBox size shared by every built-in icon. */
#define BRUCE_ICON_SIZE 24

/*
 * Return the SVG path-data string for a built-in icon, or NULL if `name` is
 * not recognized.
 *
 * Recognized names (case-sensitive):
 *   "wifi"       - Wi-Fi signal icon
 *   "bluetooth"  - Bluetooth rune
 *   "bt"         - alias for "bluetooth"
 *   "ir"         - infrared / remote icon
 *   "folder"     - folder icon
 *   "files"      - alias for "folder"
 *   "terminal"   - terminal / console icon
 *   "clock"      - analog clock icon
 *   "settings"   - cog / settings icon
 *   "cog"        - alias for "settings"
 *   "selftest"   - test-tube icon
 *   "test-tube"  - alias for "selftest"
 *   "apps"       - 2x2 grid of squares
 *
 * The returned string is owned by Core; the caller must not free or modify it.
 */
const char *icon__get(const char *name);
