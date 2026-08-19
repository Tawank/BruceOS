#pragma once

/*
 * Decodes an already-fetched, still-encoded image and draws it scaled to fit
 * within a caller-chosen box -- something core_sdk/image.h's own `fit`/`center`
 * options don't offer (they scale to the caller's *whole* display viewport,
 * which is right for a full-screen image viewer but not for one inline
 * picture among several on a page). Kept as its own small file since it's a
 * self-contained piece of math/decoding that browser_render.c's layout
 * visitor just calls into.
 */

#include <stddef.h>

#include "core_sdk/display.h"
#include "core_sdk/result.h"

/* Decodes `data`/`len` (JPEG/PNG/GIF, as returned by browser_image_cache.h)
 * and draws it at `(x, y)`, uniformly scaled down (never up) to fit within
 * `box_width` x `box_height`, and centered within that box. `background`
 * fills any transparent source pixels and the box's unused letterbox area.
 * On success, `*out_drawn_width`/`*out_drawn_height` (if non-NULL) report the
 * actual on-screen size, which may be smaller than the box. */
bruce_result_t browser_image_draw__fit(
    const void *data, size_t len, int x, int y, int box_width, int box_height, bruce_display_color_t background,
    int *out_drawn_width, int *out_drawn_height
);
