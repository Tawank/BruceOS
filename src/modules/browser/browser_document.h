#pragma once

/*
 * In-memory representation of one fetched page: a flat, ordered list of
 * content items plus the text/link/image data they point into. This is
 * deliberately not a DOM -- see browser_page.h for how it's built directly
 * from core_sdk/html.h's streaming parser events. browser_layout.h walks it
 * to word-wrap and position each item for drawing.
 *
 * All storage here is capped (see the BROWSER_MAX_* constants below): a page
 * that exceeds a cap is simply truncated rather than failing outright, and
 * `truncated` is set so the caller can say so.
 */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

#define BROWSER_URL_MAX 400
#define BROWSER_ALT_MAX 96
#define BROWSER_TITLE_MAX 96

/* Caps chosen to keep one document's worst-case footprint in the tens of KB:
 * BROWSER_MAX_TEXT_BYTES text pool + BROWSER_MAX_ITEMS * sizeof(item) +
 * link/image arrays, comfortably affordable from PSRAM/internal heap on every
 * supported board. */
#define BROWSER_MAX_TEXT_BYTES (48u * 1024u)
#define BROWSER_MAX_ITEMS 1024u
#define BROWSER_MAX_LINKS 256u
#define BROWSER_MAX_IMAGES 64u

typedef enum {
    BROWSER_ITEM_TEXT,
    BROWSER_ITEM_IMAGE,
    BROWSER_ITEM_LINE_BREAK,
    BROWSER_ITEM_PARAGRAPH_BREAK,
} browser_item_kind_t;

typedef struct {
    browser_item_kind_t kind;
    size_t text_offset; /* BROWSER_ITEM_TEXT: byte offset into text_pool. */
    size_t text_len;
    int heading_level; /* 0 = normal text, 1-6 = <h1>-<h6>. */
    int link_index;    /* -1 when not part of a link, else index into links[]. */
    int image_index;   /* BROWSER_ITEM_IMAGE only; index into images[]. */
} browser_item_t;

typedef struct {
    char url[BROWSER_URL_MAX];
} browser_link_t;

typedef struct {
    char url[BROWSER_URL_MAX];
    char alt[BROWSER_ALT_MAX];
} browser_image_ref_t;

typedef struct {
    char *text_pool;
    size_t text_pool_len;
    size_t text_pool_cap;

    browser_item_t *items;
    size_t item_count;
    size_t item_cap;

    browser_link_t *links;
    size_t link_count;
    size_t link_cap;

    browser_image_ref_t *images;
    size_t image_count;
    size_t image_cap;

    char title[BROWSER_TITLE_MAX];
    char url[BROWSER_URL_MAX]; /* This page's own resolved URL. */
    bool truncated;            /* A cap above was hit; the page is incomplete. */
} browser_document_t;

bruce_result_t browser_document__create(browser_document_t **out_doc);
void browser_document__destroy(browser_document_t *doc);

/* Clears content for reuse across navigations without releasing the backing
 * arrays, so repeated navigation doesn't repeatedly reallocate from scratch. */
void browser_document__reset(browser_document_t *doc);

void browser_document__set_url(browser_document_t *doc, const char *url);
void browser_document__set_title(browser_document_t *doc, const char *title, size_t len);

/* Appends a text run. No-op once BROWSER_MAX_TEXT_BYTES or BROWSER_MAX_ITEMS
 * is reached (sets `truncated`). */
void browser_document__add_text(browser_document_t *doc, const char *text, size_t len, int heading_level, int link_index);

/* Registers a link target and returns its index for use as add_text()'s
 * `link_index`, or -1 once BROWSER_MAX_LINKS is reached (the surrounding text
 * still renders, just without a clickable target). */
int browser_document__add_link(browser_document_t *doc, const char *url);

/* Registers an image reference as its own item. Silently dropped once
 * BROWSER_MAX_IMAGES or BROWSER_MAX_ITEMS is reached. */
void browser_document__add_image(browser_document_t *doc, const char *url, const char *alt, size_t alt_len);

void browser_document__add_break(browser_document_t *doc, bool paragraph);
