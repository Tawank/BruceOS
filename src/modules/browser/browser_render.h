#pragma once

/*
 * Draws the browser's top chrome bar (back/forward + URL) and the current
 * document body, scrolled and with its currently-selected link highlighted.
 * Word-wrap positioning comes from browser_layout.h; this file is the only
 * one in the module that calls core_sdk/display.h.
 */

#include <stdbool.h>

#include "browser_document.h"
#include "browser_history.h"
#include "browser_image_cache.h"
#include "core_sdk/result.h"

/* User-adjustable font size, as a delta from the default scale (0) applied on
 * top of every heading level's base scale -- see
 * browser_layout__heading_scale(). Adjusted in 0.5 steps and clamped to this
 * range on every +/- press; -1.5 bottoms body text out at scale 0.5 (the
 * smallest browser_layout__heading_scale() allows), +3 is about as large as
 * fits without every word wrapping onto its own line at this display
 * width. */
#define BROWSER_FONT_SCALE_MIN (-1.5f)
#define BROWSER_FONT_SCALE_MAX 3.0f

typedef struct {
    int scroll_y;       /* Content pixels scrolled past the top. */
    int selected_link;  /* Index into doc->links, or -1 for none selected. */
    int selected_image; /* Index into doc->images, or -1 for none selected. Mutually
                          * exclusive with selected_link -- a row is either a link row,
                          * an image row (images are always alone on their own row --
                          * see browser_document.h), or neither. */
    int row_y;           /* Content y of the row Up/Down navigation is on, or -1 before the first move. */
    float font_scale;     /* User's +/- font size preference, in 0.5 steps; see BROWSER_FONT_SCALE_MIN/MAX above. */
} browser_view_state_t;

/* Pixel height of the top chrome bar, derived from the active font. */
int browser_render__chrome_height(void);

/* Usable content width for word-wrap layout (viewport width minus margins). */
int browser_render__content_width(void);

/* Pixel height of the content viewport below the chrome bar (never negative). */
int browser_render__view_height(void);

/* Total laid-out height of `doc` at the current content width and
 * `font_scale` (see BROWSER_FONT_SCALE_MIN/MAX above). `image_cache` is
 * peek()ed (never fetched) so an already-loaded image's row reflects its
 * real fitted height rather than the unloaded placeholder -- pass the same
 * cache used to draw `doc`, or NULL to always assume the placeholder. */
int browser_render__content_height(
    const browser_document_t *doc, float font_scale, browser_image_cache_t *image_cache
);

/* Largest `scroll_y` that still shows content, i.e. content_height minus the
 * viewport height below the chrome bar (never negative). See
 * browser_render__content_height() for `image_cache`. */
int browser_render__max_scroll(const browser_document_t *doc, float font_scale, browser_image_cache_t *image_cache);

/* Returns the content-local y position of the first drawable item at or
 * after `item_index`, clamped to the document's bottom. See
 * browser_render__content_height() for `image_cache`. */
int browser_render__item_y(
    const browser_document_t *doc, size_t item_index, float font_scale, browser_image_cache_t *image_cache
);

#define BROWSER_ROW_MAX_LINKS 16

/* One word-wrapped content row (everything sharing a single layout y), as
 * needed to drive line-by-line Up/Down navigation: see
 * browser_app__move_line() in browser_app.c. Distinct links within the row
 * are listed left to right; a row packing more than BROWSER_ROW_MAX_LINKS
 * distinct links onto one line (implausible at any real viewport width) just
 * isn't reachable by name past the first that many -- it's still drawn and
 * still clickable once selected some other way. image_index is -1 unless
 * this row is an image (images always get their own row -- see
 * browser_document.h -- so it's never alongside a link). */
typedef struct {
    int y;
    int line_height;
    int link_indices[BROWSER_ROW_MAX_LINKS];
    int link_count;
    int image_index;
} browser_render_row_t;

/* Finds the content row adjacent to `after_y` in `direction`: the row with
 * the smallest y greater than `after_y` (direction > 0), or the largest y
 * less than `after_y` (direction < 0). Pass `after_y` < 0 to instead get the
 * very first (direction > 0) or very last (direction < 0) row of the
 * document. Returns false, `*out_row` untouched, if there's no such row
 * (already at an end, or an empty document). See
 * browser_render__content_height() for `image_cache`. */
bool browser_render__find_row(
    const browser_document_t *doc, int after_y, int direction, float font_scale, browser_image_cache_t *image_cache,
    browser_render_row_t *out_row
);

/* Draws one full frame: chrome bar plus the visible slice of `doc` for
 * `view`. Fetches/decodes any inline images newly scrolled into view through
 * `image_cache`. Requires GUI context (a foreground viewport). */
bruce_result_t browser_render__draw(
    const browser_document_t *doc, const browser_view_state_t *view, const browser_history_t *history,
    browser_image_cache_t *image_cache
);

/* Draws an empty loading view with the URL chrome visible. `progress` is
 * clamped to 0..100 and fills the blue-gray chrome from left to right. */
bruce_result_t browser_render__draw_loading(
    const browser_document_t *doc, const browser_history_t *history, int progress
);
