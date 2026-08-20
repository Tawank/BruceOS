#include "browser_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/html.h"
#include "core_sdk/http.h"

#define BROWSER_PAGE_MAX_RESPONSE_BYTES (512u * 1024u)
#define BROWSER_PAGE_USER_AGENT "Bruce-Browser/1.0"

typedef struct {
    browser_document_t *doc;
    bruce_html_parser_t *parser;
    int current_link;
    int current_heading;
    size_t received;
    browser_page_progress_cb_t progress_cb;
    void *progress_context;
} browser_page_event_state_t;

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
    }
}

static bruce_result_t browser_page__on_chunk(const void *data, size_t data_len, void *context) {
    browser_page_event_state_t *state = context;
    bruce_result_t result = html__parser_feed(state->parser, data, data_len);
    if (result == BRUCE_OK) {
        state->received += data_len;
        if (state->progress_cb != NULL) state->progress_cb(state->received, state->progress_context);
    }
    return result;
}

bool browser_page__normalize_url(const char *raw_url, char *out_url, size_t out_capacity) {
    if (raw_url == NULL || raw_url[0] == '\0' || out_url == NULL) return false;
    int written = strstr(raw_url, "://") != NULL ? snprintf(out_url, out_capacity, "%s", raw_url)
                                                   : snprintf(out_url, out_capacity, "http://%s", raw_url);
    return written >= 0 && (size_t)written < out_capacity;
}

bruce_result_t browser_page__fetch(
    const char *url, browser_document_t *doc, int *out_status_code, browser_page_progress_cb_t progress_cb,
    void *progress_context
) {
    if (url == NULL || url[0] == '\0' || doc == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (out_status_code != NULL) *out_status_code = 0;

    browser_document__reset(doc);
    browser_document__set_url(doc, url);

    browser_page_event_state_t state = {
        .doc = doc,
        .current_link = -1,
        .current_heading = 0,
        .progress_cb = progress_cb,
        .progress_context = progress_context,
    };
    bruce_html_parser_t *parser = NULL;
    bruce_result_t result = html__parser_create(url, browser_page__on_event, &state, &parser);
    if (result != BRUCE_OK) return result;
    state.parser = parser;

    const char *headers[] = {"User-Agent", BROWSER_PAGE_USER_AGENT};
    bruce_http_request_t request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = 1,
        .max_response_bytes = BROWSER_PAGE_MAX_RESPONSE_BYTES,
        .on_response_chunk = browser_page__on_chunk,
        .response_chunk_context = &state,
    };
    bruce_http_response_t response = {0};
    result = http__request(&request, &response);
    if (result == BRUCE_OK) {
        if (out_status_code != NULL) *out_status_code = response.status_code;
        (void)html__parser_finish(parser);
        /* The document won't grow again until the next navigation resets it
         * -- release whatever doubling-growth slack the items array is
         * still holding from parsing this page. */
        browser_document__shrink_to_fit(doc);
    }
    http__response_free(&response);
    html__parser_destroy(parser);
    return result;
}
