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
 *
 * text_pool, links, and images are only ever appended to during parsing,
 * then read-only afterward -- so they live in a memory__external_malloc()
 * allocation (PSRAM, or swap carved out of flash where there's no PSRAM)
 * instead of the process-owned internal heap, the same way core/http/http.c
 * keeps a response body off the internal heap once it's known to be
 * sizeable. That leaves more internal RAM free for things that must live
 * there, in particular mbedTLS's own per-request TLS buffers during an inline image
 * fetch. items stays on the internal heap: it's walked byte-by-byte on
 * *every* draw (word-wrap layout re-runs on every redraw, not just once per
 * navigation, and does so 2-3 times per frame -- see browser_render.c) and
 * is the one structure where flash-mmap read latency would risk visible
 * scroll jank; images and links are only touched when a link or image token
 * is actually being drawn, a much smaller fraction of a walk.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BROWSER_URL_MAX 400
#define BROWSER_ALT_MAX 96
#define BROWSER_TITLE_MAX 96
#define BROWSER_ANCHOR_MAX 96

/* Caps chosen to keep one document's worst-case footprint in the tens of KB:
 * BROWSER_MAX_TEXT_BYTES text pool + BROWSER_MAX_ITEMS * sizeof(item) +
 * link/image arrays, comfortably affordable from PSRAM/internal heap on every
 * supported board. */
#define BROWSER_MAX_TEXT_BYTES (48u * 1024u)
#define BROWSER_MAX_ITEMS 1024u
#define BROWSER_MAX_LINKS 256u
#define BROWSER_MAX_IMAGES 64u
#define BROWSER_MAX_ANCHORS 128u

typedef enum {
    BROWSER_ITEM_TEXT,
    BROWSER_ITEM_IMAGE,
    BROWSER_ITEM_LINE_BREAK,
    BROWSER_ITEM_PARAGRAPH_BREAK,
} browser_item_kind_t;

/* Packed to 10 bytes (down from 24 as plain `int`/`size_t` fields) -- this is
 * the one array that's never moved off the internal heap (see the comment
 * above), and BROWSER_MAX_ITEMS=1024 of them is otherwise the single largest
 * internal-RAM consumer the browser module has. Every field's range is
 * already bounded by one of the BROWSER_MAX_* caps below, so the narrower
 * types lose no information -- see the _Static_assert()s further down that
 * fail the build if a cap is ever widened past what a field can hold. */
typedef struct {
    uint8_t kind;          /* browser_item_kind_t. */
    int8_t heading_level;  /* 0 = normal text, 1-6 = <h1>-<h6>. */
    int16_t link_index;    /* -1 when not part of a link, else index into links[]. */
    int16_t image_index;   /* BROWSER_ITEM_IMAGE only; index into images[]. */
    uint16_t text_offset;  /* BROWSER_ITEM_TEXT: byte offset into text_pool. */
    uint16_t text_len;
} browser_item_t;

_Static_assert(BROWSER_MAX_TEXT_BYTES - 1 <= 0xFFFFu, "text_offset/text_len no longer fit uint16_t");
_Static_assert(BROWSER_MAX_LINKS <= 0x7FFFu, "link_index no longer fits int16_t");
_Static_assert(BROWSER_MAX_IMAGES <= 0x7FFFu, "image_index no longer fits int16_t");

typedef struct {
    char url[BROWSER_URL_MAX];
} browser_link_t;

typedef struct {
    char url[BROWSER_URL_MAX];
    char alt[BROWSER_ALT_MAX];
} browser_image_ref_t;

typedef struct {
    char name[BROWSER_ANCHOR_MAX];
    uint16_t item_index;
} browser_anchor_t;

/* Named regions modules/browser's own keybindings jump straight to (see
 * browser_app.c's 'a'/'n' handling) -- mirrors bruce_html_landmark_t
 * (core_sdk/html.h) one-to-one; kept as its own type so this header doesn't
 * have to depend on the HTML parser's. */
typedef enum {
    BROWSER_LANDMARK_MAIN,
    BROWSER_LANDMARK_ARTICLE,
    BROWSER_LANDMARK_NAV,
    BROWSER_LANDMARK_FOOTER,
} browser_landmark_kind_t;

typedef struct {
    const char *text_pool; /* Externally backed -- see the comment above. */
    size_t text_pool_len;
    size_t text_pool_cap;

    browser_item_t *items;
    size_t item_count;
    size_t item_cap;

    const browser_link_t *links; /* Externally backed -- see the comment above. */
    size_t link_count;
    size_t link_cap;

    const browser_image_ref_t *images; /* Externally backed -- see the comment above. */
    size_t image_count;
    size_t image_cap;

    const browser_anchor_t *anchors; /* Externally backed -- see the comment above. */
    size_t anchor_count;
    size_t anchor_cap;

    /* item_index of the first <main>/<article>/<nav>/<footer> seen while
     * parsing, or -1 if the page has none -- see
     * browser_document__add_landmark(). */
    int main_item_index;
    int article_item_index;
    int nav_item_index;
    int footer_item_index;

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
 * is reached (sets `truncated`). An exact repeat of the item immediately
 * before this one (one intervening break allowed) is folded into it instead
 * of appended separately -- see browser_document__fold_repeated_text() in the
 * .c file. */
void browser_document__add_text(browser_document_t *doc, const char *text, size_t len, int heading_level, int link_index);

/* Registers a link target and returns its index for use as add_text()'s
 * `link_index`, or -1 once BROWSER_MAX_LINKS is reached (the surrounding text
 * still renders, just without a clickable target). */
int browser_document__add_link(browser_document_t *doc, const char *url);

/* Registers an image reference as its own item. Silently dropped once
 * BROWSER_MAX_IMAGES or BROWSER_MAX_ITEMS is reached. */
void browser_document__add_image(browser_document_t *doc, const char *url, const char *alt, size_t alt_len);

void browser_document__add_anchor(browser_document_t *doc, const char *name, size_t len);
bool browser_document__find_anchor(const browser_document_t *doc, const char *name, size_t *out_item_index);

/* Records the item index of the *first* <main>/<article>/<nav>/<footer> of a
 * given kind seen while parsing; later ones of the same kind are ignored.
 * Read back directly via
 * doc->main_item_index/article_item_index/nav_item_index/footer_item_index
 * (-1 if that kind never appeared). */
void browser_document__add_landmark(browser_document_t *doc, browser_landmark_kind_t kind);

void browser_document__add_break(browser_document_t *doc, bool paragraph);

/* Trims the items array's capacity down to its actual item_count, releasing
 * whatever doubling-growth slack is left over (up to ~2x item_count worth,
 * right after the last add_*() call that grew it). Call once after a page is
 * fully parsed -- there's no reason to keep spare growth headroom around for
 * a document that won't be added to again until the next navigation resets
 * it. A no-op if there's nothing to reclaim. */
void browser_document__shrink_to_fit(browser_document_t *doc);
