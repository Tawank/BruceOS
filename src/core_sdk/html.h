#pragma once

/*
 * Public streaming HTML-to-content extractor (Core SDK surface).
 *
 * This is intentionally not a DOM: ESP32 RAM is too tight to hold a parsed
 * tree of an arbitrary web page. Instead the parser is a byte-at-a-time SAX
 * style tokenizer -- feed it bytes as they arrive over HTTP (see
 * `bruce_http_response_chunk_cb_t` in core_sdk/http.h) and it reports a small
 * vocabulary of content events as it recognizes them: plain text runs,
 * link/heading boundaries, image references, line/paragraph breaks, and
 * <main>/<article>/<nav> landmark starts. `<script>`, `<style>`, and markup
 * the vocabulary below doesn't describe are silently skipped rather than
 * reported.
 *
 * Any module that wants "the readable content of an HTML page" -- a browser,
 * a feed reader, a page-title lookup -- can reuse this instead of writing its
 * own tag stripper.
 *
 * Text is whitespace-collapsed (a run of spaces/tabs/newlines becomes one
 * space) and HTML entities are decoded. `href`/`src` attribute values are
 * resolved to absolute URLs against the parser's base URL before being
 * reported.
 */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BRUCE_HTML_EVENT_TITLE,          /* `text` is the decoded <title> content. */
    BRUCE_HTML_EVENT_TEXT,           /* `text` is a whitespace-collapsed text run. */
    BRUCE_HTML_EVENT_LINK_START,     /* `text` is the link's resolved absolute URL. */
    BRUCE_HTML_EVENT_LINK_END,
    BRUCE_HTML_EVENT_IMAGE,          /* `text` is the image's resolved absolute URL;
                                       * `alt` is its optional alt text. */
    BRUCE_HTML_EVENT_ANCHOR,         /* `text` is an element's decoded `id` attribute. */
    BRUCE_HTML_EVENT_HEADING_START,  /* `value` is the heading level, 1-6. */
    BRUCE_HTML_EVENT_HEADING_END,
    BRUCE_HTML_EVENT_LINE_BREAK,     /* <br>, or an <li> closing (kept snug against its siblings). */
    BRUCE_HTML_EVENT_PARAGRAPH_BREAK,/* End of a block element such as <p>, <div>, or a <ul>/<ol>. */
    BRUCE_HTML_EVENT_LANDMARK_START, /* <main>/<article>/<nav> open; `value` is a bruce_html_landmark_t. */
    BRUCE_HTML_EVENT_LIST_ITEM_START,/* <li> open; `value` is its <ul>/<ol> nesting depth (1 = not nested). */
} bruce_html_event_type_t;

/* Named regions a reader may want to jump straight to, reported by
 * BRUCE_HTML_EVENT_LANDMARK_START's `value`. */
typedef enum {
    BRUCE_HTML_LANDMARK_MAIN,
    BRUCE_HTML_LANDMARK_ARTICLE,
    BRUCE_HTML_LANDMARK_NAV,
} bruce_html_landmark_t;

typedef struct {
    bruce_html_event_type_t type;
    /* Borrowed pointers, valid only for the duration of the callback. Never
     * NUL-terminated by contract; always use the paired _len. */
    const char *text;
    size_t text_len;
    const char *alt;
    size_t alt_len;
    int value;
} bruce_html_event_t;

/* Called synchronously as the parser recognizes each event. `text`/`alt`
 * pointers are only valid for the duration of the call; copy anything that
 * must outlive it. */
typedef void (*bruce_html_event_cb_t)(const bruce_html_event_t *event, void *context);

typedef struct bruce_html_parser bruce_html_parser_t;

/* Creates a streaming parser. `base_url` is used to resolve relative
 * `href`/`src` attributes and is copied; it may be NULL if the caller only
 * wants text/structure events (relative links/images are then reported
 * unresolved, exactly as written in the markup). */
bruce_result_t html__parser_create(
    const char *base_url, bruce_html_event_cb_t callback, void *context, bruce_html_parser_t **out_parser
);

/* Feeds the next chunk of HTML bytes. May be called repeatedly with partial
 * data (e.g. directly from an HTTP response chunk callback); the parser
 * carries incomplete tags/entities across calls. */
bruce_result_t html__parser_feed(bruce_html_parser_t *parser, const void *data, size_t len);

/* Flushes any pending text run and closes out open elements. Call once after
 * the last html__parser_feed(). */
bruce_result_t html__parser_finish(bruce_html_parser_t *parser);

void html__parser_destroy(bruce_html_parser_t *parser);

/* Resolves `ref` (absolute, scheme-relative, absolute-path, or relative)
 * against `base_url` into an absolute URL written to `out_url`. Returns false
 * if the result would not fit `out_capacity` or `ref`/`base_url` is
 * malformed. `ref` is returned unchanged (copied) when it is already
 * absolute. */
bool html__resolve_url(const char *base_url, const char *ref, char *out_url, size_t out_capacity);

#ifdef __cplusplus
}
#endif
