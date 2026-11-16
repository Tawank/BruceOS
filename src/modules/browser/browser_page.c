#include "browser_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/html.h"
#include "core_sdk/http.h"
#include "core_sdk/memory.h"

#define BROWSER_PAGE_MAX_RESPONSE_BYTES (512u * 1024u)
#define BROWSER_PAGE_USER_AGENT "Bruce-Browser/1.0"
/* Per load_step(): enough to keep up with the network, short enough that
 * input is checked again quickly on a slow device. */
#define BROWSER_PAGE_READ_CHUNK 1024u
#define BROWSER_PAGE_READS_PER_STEP 8

/* Longest indent marker browser_page__add_list_marker() will build, sized for
 * BROWSER_PAGE_MAX_INDENT levels of "  " plus "- " -- comfortably under this
 * even at that cap, but small screens can't afford deep indents anyway (see
 * BROWSER_PAGE_MAX_INDENT). */
#define BROWSER_PAGE_LIST_MARKER_MAX 16
/* Nesting levels beyond this render at the same indent as this one -- a
 * small screen has no room to keep stepping in further, and a reader can
 * still tell items apart by which heading they follow. */
#define BROWSER_PAGE_MAX_INDENT 3

typedef struct {
    browser_document_t *doc;
    bruce_html_parser_t *parser;
    int current_link;
    int current_heading;
} browser_page_event_state_t;

struct browser_page_load {
    browser_page_event_state_t state;
    bruce_http_job_id_t job;
    size_t received;
    int status_code;
    bool done;
    bruce_result_t result; /* Once done. */
};

/* <li> nesting depth (BRUCE_HTML_EVENT_LIST_ITEM_START's `value`) is 1 for a
 * top-level item, so it isn't indented; a nested list's items get a leading
 * " - " marker, one level deeper widening it by two spaces each, capped at
 * BROWSER_PAGE_MAX_INDENT so a deeply-nested page (e.g. a wiring-diagram tree
 * several boards deep) doesn't run the marker off a small screen. Emitted as
 * an ordinary text item -- see browser_document__add_text() -- so it just
 * flows into the line ahead of the <li>'s real content with no other layout
 * changes needed. */
static void browser_page__add_list_marker(browser_document_t *doc, int depth) {
    int indent = depth > 1 ? depth - 1 : 0;
    if (indent <= 0) return;
    if (indent > BROWSER_PAGE_MAX_INDENT) indent = BROWSER_PAGE_MAX_INDENT;

    char marker[BROWSER_PAGE_LIST_MARKER_MAX];
    size_t n = 0;
    for (int i = 0; i < 2 * indent - 1 && n < sizeof(marker) - 3u; ++i) marker[n++] = ' ';
    marker[n++] = '-';
    marker[n++] = ' ';
    browser_document__add_text(doc, marker, n, 0, -1);
}

static void browser_page__on_event(const bruce_html_event_t *event, void *context) {
    browser_page_event_state_t *state = context;
    switch (event->type) {
    case BRUCE_HTML_EVENT_TITLE:
        browser_document__set_title(state->doc, event->text, event->text_len);
        break;
    case BRUCE_HTML_EVENT_TEXT:
        browser_document__add_text(
            state->doc, event->text, event->text_len, state->current_heading, state->current_link
        );
        break;
    case BRUCE_HTML_EVENT_LINK_START:
        state->current_link = browser_document__add_link(state->doc, event->text);
        break;
    case BRUCE_HTML_EVENT_LINK_END:
        state->current_link = -1;
        break;
    case BRUCE_HTML_EVENT_IMAGE:
        /* Images are always block-level in this renderer: force them onto
         * their own line regardless of what the markup did. */
        browser_document__add_break(state->doc, true);
        browser_document__add_image(state->doc, event->text, event->alt, event->alt_len);
        browser_document__add_break(state->doc, true);
        break;
    case BRUCE_HTML_EVENT_ANCHOR:
        browser_document__add_anchor(state->doc, event->text, event->text_len);
        break;
    case BRUCE_HTML_EVENT_HEADING_START:
        /* Likewise, always start a heading on a fresh line even if the
         * source markup ran it straight on from preceding loose text. */
        browser_document__add_break(state->doc, true);
        state->current_heading = event->value;
        break;
    case BRUCE_HTML_EVENT_HEADING_END:
        state->current_heading = 0;
        break;
    case BRUCE_HTML_EVENT_LINE_BREAK:
        browser_document__add_break(state->doc, false);
        break;
    case BRUCE_HTML_EVENT_PARAGRAPH_BREAK:
        browser_document__add_break(state->doc, true);
        break;
    case BRUCE_HTML_EVENT_LANDMARK_START: {
        /* Same reasoning as heading/image above: a <main>/<article>/<nav>/
         * <footer> commonly opens with no line break of its own in the
         * markup (e.g. a secondary <nav> for a table of contents sitting
         * right after a nav item's own link) -- force one so it doesn't run
         * on from whatever came before it. */
        browser_document__add_break(state->doc, true);
        browser_landmark_kind_t kind;
        switch ((bruce_html_landmark_t)event->value) {
        case BRUCE_HTML_LANDMARK_MAIN: kind = BROWSER_LANDMARK_MAIN; break;
        case BRUCE_HTML_LANDMARK_ARTICLE: kind = BROWSER_LANDMARK_ARTICLE; break;
        case BRUCE_HTML_LANDMARK_NAV: kind = BROWSER_LANDMARK_NAV; break;
        default: kind = BROWSER_LANDMARK_FOOTER; break;
        }
        browser_document__add_landmark(state->doc, kind);
        break;
    }
    case BRUCE_HTML_EVENT_LIST_ITEM_START:
        /* Each <li> starts its own line -- a plain one, not a paragraph
         * break, matching how BRUCE_HTML_EVENT_LINE_BREAK now closes it too
         * (see html.h) so list items stay snug against each other -- then
         * gets an indent marker if it's nested. This is the only source of
         * that leading break for the *first* <li> in a list (nothing else
         * forces one between, say, a "Table of contents" label and the list
         * under it); for every later sibling it just folds into the one its
         * predecessor's own close already emitted. */
        browser_document__add_break(state->doc, false);
        browser_page__add_list_marker(state->doc, event->value);
        break;
    }
}

bool browser_page__normalize_url(const char *raw_url, char *out_url, size_t out_capacity) {
    if (raw_url == NULL || raw_url[0] == '\0' || out_url == NULL) return false;
    int written = strstr(raw_url, "://") != NULL ? snprintf(out_url, out_capacity, "%s", raw_url)
                                                   : snprintf(out_url, out_capacity, "http://%s", raw_url);
    return written >= 0 && (size_t)written < out_capacity;
}

static void browser_page__finish(browser_page_load_t *load) {
    if (load->done) return;
    load->done = true;
    (void)html__parser_finish(load->state.parser);
    /* The document won't grow again until the next navigation resets it --
     * release whatever doubling-growth slack the items array still holds. */
    browser_document__shrink_to_fit(load->state.doc);
}

bruce_result_t
browser_page__load_start(const char *url, browser_document_t *doc, browser_page_load_t **out_load) {
    if (url == NULL || url[0] == '\0' || doc == NULL || out_load == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_load = NULL;
    browser_document__reset(doc);
    browser_document__set_url(doc, url);

    browser_page_load_t *load = memory__malloc(sizeof(*load));
    if (load == NULL) return BRUCE_ERR_NO_MEMORY;
    *load = (browser_page_load_t){.state = {.doc = doc, .current_link = -1}};
    bruce_result_t result =
        html__parser_create(url, browser_page__on_event, &load->state, &load->state.parser);
    if (result != BRUCE_OK) {
        memory__free(load);
        return result;
    }

    const char *headers[] = {"User-Agent", BROWSER_PAGE_USER_AGENT};
    bruce_http_request_t request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = 1,
        .max_response_bytes = BROWSER_PAGE_MAX_RESPONSE_BYTES,
        .stream = true,
    };
    result = http__request_start(&request, &load->job);
    if (result != BRUCE_OK) {
        html__parser_destroy(load->state.parser);
        memory__free(load);
        return result;
    }
    *out_load = load;
    return BRUCE_OK;
}

bruce_result_t
browser_page__load_step(browser_page_load_t *load, int *out_status_code, size_t *out_received) {
    if (load == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result = load->done ? load->result : BRUCE_ERR_BUSY;
    char chunk[BROWSER_PAGE_READ_CHUNK];
    for (int i = 0; i < BROWSER_PAGE_READS_PER_STEP && !load->done; ++i) {
        size_t len = 0;
        result = http__request_read(load->job, chunk, sizeof(chunk), &len, &load->status_code);
        if (result == BRUCE_OK && len > 0) {
            result = html__parser_feed(load->state.parser, chunk, len);
            load->received += len;
            if (result == BRUCE_OK) {
                result = BRUCE_ERR_BUSY;
                continue;
            }
            http__request_cancel(load->job);
        }
        if (result != BRUCE_ERR_BUSY) {
            load->result = result;
            browser_page__finish(load);
        }
        break;
    }
    if (out_status_code != NULL) *out_status_code = load->status_code;
    if (out_received != NULL) *out_received = load->received;
    return result;
}

void browser_page__load_end(browser_page_load_t *load) {
    if (load == NULL) return;
    if (!load->done) {
        http__request_cancel(load->job);
        browser_page__finish(load);
    }
    html__parser_destroy(load->state.parser);
    memory__free(load);
}
