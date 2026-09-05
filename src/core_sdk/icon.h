#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Built-in icons.
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

/** Buffer size for an icon name returned by icon__pick(). */
#define BRUCE_ICON_NAME_MAX 32

/**
 * @brief Shows a picker listing every built-in icon and returns the one chosen.
 *
 * A dialog__choice() list with one row per icon__get()-recognized name, each
 * drawn with its own icon so the list doubles as a preview. Falls back to a
 * plain-text list of names outside a GUI context, same as any other
 * dialog__* call.
 *
 * @param title Optional short title shown at the top of the picker.
 * @param current_icon_name Optional icon name to preselect, or NULL/"" for none.
 * @param allow_none When true, an extra leading "None" row clears out_icon_name instead of picking one.
 * @param out_icon_name Receives the chosen icon name ("" if "None" was picked). Left untouched on a
 * non-BRUCE_OK return (e.g. the picker was cancelled).
 * @param out_icon_name_size Size of out_icon_name in bytes.
 */
bruce_result_t icon__pick(
    const char *title, const char *current_icon_name, bool allow_none, char *out_icon_name,
    size_t out_icon_name_size
);
