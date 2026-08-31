#pragma once

/*
 * Pure word-wrap layout for a browser_document_t: no display_sdk calls here,
 * just arithmetic over caller-supplied font metrics. This lets
 * browser_render.c reuse the exact same pass both to actually draw the
 * visible portion of a page and to measure total content height / locate a
 * link's on-screen position for scrolling to it.
 */

#include <stddef.h>

#include "browser_document.h"
#include "browser_image_cache.h"

/* Reserved height for an inline image's layout box before it's been loaded
 * (see browser_image_cache.h: images fetch only on an explicit Select press,
 * so most of the time there is nothing decoded yet to size the row from) --
 * see browser_render.c's "Select to load" placeholder. Once an image is in
 * `image_cache`, browser_layout__walk() sizes its row to the image's actual
 * fitted height instead (see browser_app.c's browser_app__load_image(),
 * which fits square/vertical images up to the full viewport height and
 * leaves horizontal ones sized by width, same as before). */
#define BROWSER_IMAGE_BOX_HEIGHT 72

typedef struct {
    size_t item_index;
    int x;
    int y; /* Top-left, in content-local pixels; y=0 is the document's top. */
    int line_height;
    const char *text; /* Borrowed pointer into the document's text pool; NULL for an image token. */
    size_t text_len;
    int heading_level;
    int link_index;  /* -1 when this token isn't part of a link. */
    int image_index; /* -1 for a text token; index into doc->images for an image token. */
} browser_layout_token_t;

typedef void (*browser_layout_visitor_t)(const browser_layout_token_t *token, void *context);

/* The display__set_text_size() multiplier for a heading level (0 = body/link
 * text), adjusted by `font_scale_delta` (the user's +/- font size preference,
 * in 0.5 steps, 0 = default) and clamped to a sane [0.5, 8] range. */
float browser_layout__heading_scale(int heading_level, float font_scale_delta);

/* Word-wraps `doc` to `width` px using the given native (unscaled) font cell
 * size and `font_scale_delta` (see browser_layout__heading_scale above),
 * calling `visitor` once per positioned word or image, in document order.
 * `image_cache` is peek()ed (never fetched) to size an already-loaded
 * image's row to its real fitted height; pass NULL to always use the
 * BROWSER_IMAGE_BOX_HEIGHT placeholder instead. Returns the total laid-out
 * content height in pixels. */
int browser_layout__walk(
    const browser_document_t *doc, int width, int char_width, int char_height, float font_scale_delta,
    browser_image_cache_t *image_cache, browser_layout_visitor_t visitor, void *context
);
