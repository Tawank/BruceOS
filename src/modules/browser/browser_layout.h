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

/* Fixed reserved height for an inline image's layout box; the actual decoded
 * image is scaled to fit within (available width) x BROWSER_IMAGE_BOX_HEIGHT
 * -- see browser_render.c. */
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
 * Returns the total laid-out content height in pixels. */
int browser_layout__walk(
    const browser_document_t *doc, int width, int char_width, int char_height, float font_scale_delta,
    browser_layout_visitor_t visitor, void *context
);
