#pragma once

/*
 * Draws the browser's top chrome bar (back/forward + URL) and the current
 * document body, scrolled and with its currently-selected link highlighted.
 * Word-wrap positioning comes from browser_layout.h; this file is the only
 * one in the module that calls core_sdk/display.h.
 */

#include "browser_document.h"
#include "browser_history.h"
#include "browser_image_cache.h"
#include "core_sdk/result.h"

typedef struct {
    int scroll_y;       /* Content pixels scrolled past the top. */
    int selected_link;  /* Index into doc->links, or -1 for none selected. */
} browser_view_state_t;

/* Pixel height of the top chrome bar, derived from the active font. */
int browser_render__chrome_height(void);

/* Usable content width for word-wrap layout (viewport width minus margins). */
int browser_render__content_width(void);

/* Pixel height of the content viewport below the chrome bar (never negative). */
int browser_render__view_height(void);

/* Total laid-out height of `doc` at the current content width. */
int browser_render__content_height(const browser_document_t *doc);

/* Largest `scroll_y` that still shows content, i.e. content_height minus the
 * viewport height below the chrome bar (never negative). */
int browser_render__max_scroll(const browser_document_t *doc);

/* Content-relative y position of link `link_index`'s first occurrence, or -1
 * if not found (e.g. an out-of-range index). When found and `out_line_height`
 * isn't NULL, also fills it with that occurrence's line height, so a caller
 * can bring the *whole* line into view rather than just its top edge (a
 * multi-scale document has lines of varying height -- a heading-sized link
 * is taller than the base line height a top-only check would assume). */
int browser_render__link_top(const browser_document_t *doc, int link_index, int *out_line_height);

/* Draws one full frame: chrome bar plus the visible slice of `doc` for
 * `view`. Fetches/decodes any inline images newly scrolled into view through
 * `image_cache`. Requires GUI context (a foreground viewport). */
bruce_result_t browser_render__draw(
    const browser_document_t *doc, const browser_view_state_t *view, const browser_history_t *history,
    browser_image_cache_t *image_cache
);
