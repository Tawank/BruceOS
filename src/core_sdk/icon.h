#pragma once

#include <stdint.h>

/**
 * @brief Built-in icon registry.
 *
 * Icons are stored as 24x24 1bpp bitmaps (72 bytes each, MSB-first, three
 * bytes per row) pre-rasterized from Material Design Icons path data. This
 * is the cheapest possible representation for filled icons: no parsing, no
 * floating point, and no working RAM at draw time. The returned pointer is
 * valid for the lifetime of the firmware and must not be freed by the
 * caller.
 */

/* Width and height shared by every built-in icon. */
#define BRUCE_ICON_SIZE 24

typedef struct {
    uint8_t width;        /* pixels, always BRUCE_ICON_SIZE for built-in icons */
    uint8_t height;       /* pixels, always BRUCE_ICON_SIZE for built-in icons */
    const uint8_t *bits;  /* 1bpp MSB-first bitmap, (width + 7) / 8 bytes per row */
} bruce_icon_t;

/**
 * @brief Return the built-in icon named `name`, or NULL if `name` is not recognized.
 *
 * Recognized names (case-sensitive): names match the SVG filenames in
 * core/icon/assets (without the .svg suffix), for example "wifi",
 * "bluetooth", "radio-handheld", and "clock-outline".
 *
 * The returned struct is owned by Core; the caller must not free or modify
 * it. Draw it with display__draw_bitmap_scaled(), e.g.:
 *
 *   const bruce_icon_t *icon = icon__get("wifi");
 *   display__draw_bitmap_scaled(x, y, icon->bits, icon->width, icon->height,
 *                               size, size, color);
 *
 * @param name Icon name, matching an SVG filename in core/icon/assets (without the .svg suffix).
 */
const bruce_icon_t *icon__get(const char *name);
