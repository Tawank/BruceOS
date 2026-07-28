#pragma once

#include <stdint.h>

/*
 * Built-in icon registry.
 *
 * Icons are stored as 24x24 1bpp bitmaps (72 bytes each, MSB-first, three
 * bytes per row) pre-rasterized from Material Design Icons path data.  This
 * is the cheapest possible representation for filled icons: no parsing, no
 * floating point, and no working RAM at draw time.  The returned pointer is
 * valid for the lifetime of the firmware and must not be freed by the caller.
 */

/* Width and height shared by every built-in icon. */
#define BRUCE_ICON_SIZE 24

typedef struct {
    uint8_t width;        /* pixels, always BRUCE_ICON_SIZE for built-in icons */
    uint8_t height;       /* pixels, always BRUCE_ICON_SIZE for built-in icons */
    const uint8_t *bits;  /* 1bpp MSB-first bitmap, (width + 7) / 8 bytes per row */
} bruce_icon_t;

/*
 * Return the built-in icon named `name`, or NULL if `name` is not recognized.
 *
 * Recognized names (case-sensitive):
 *   "wifi"       - Wi-Fi signal icon
 *   "ble"        - Bluetooth rune
 *   "remote"     - infrared / remote icon
 *   "handheld"   - radio handheld icon
 *   "folder"     - folder icon
 *   "files"      - alias for "folder"
 *   "terminal"   - terminal / console icon
 *   "clock"      - analog clock icon
 *   "settings"   - cog / settings icon
 *   "selftest"   - test-tube icon
 *   "apps"       - 3x3 grid of squares
 *
 * The returned struct is owned by Core; the caller must not free or modify it.
 * Draw it with display__draw_bitmap_scaled(), e.g.:
 *
 *   const bruce_icon_t *icon = icon__get("wifi");
 *   display__draw_bitmap_scaled(x, y, icon->bits, icon->width, icon->height,
 *                               size, size, color);
 */
const bruce_icon_t *icon__get(const char *name);
