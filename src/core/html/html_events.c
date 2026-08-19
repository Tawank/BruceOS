#include <string.h>

#include "html_internal.h"

/*
 * What a completed tag means: resolving/reporting links and images,
 * headings, and block breaks -- html_parser.c's byte-level state machine
 * hands off here via html__handle_tag_end() once it has a full tag name and
 * attribute set. See html_parser.c for the tokenizer itself.
 */

void html__finalize_attr(bruce_html_parser_t *p) {
    char *dest = NULL;
    size_t *dest_len = NULL;
    bool *dest_has = NULL;
    if (p->attr_name_len == 4 && memcmp(p->attr_name, "href", 4) == 0) {
        dest = p->href_value;
        dest_len = &p->href_len;
        dest_has = &p->has_href;
    } else if (p->attr_name_len == 3 && memcmp(p->attr_name, "src", 3) == 0) {
        dest = p->src_value;
        dest_len = &p->src_len;
        dest_has = &p->has_src;
    } else if (p->attr_name_len == 3 && memcmp(p->attr_name, "alt", 3) == 0) {
        dest = p->alt_value;
        dest_len = &p->alt_len;
        dest_has = &p->has_alt;
    }
    if (dest != NULL) {
        memcpy(dest, p->attr_value, p->attr_value_len);
        dest[p->attr_value_len] = '\0';
        *dest_len = p->attr_value_len;
        *dest_has = true;
    }
    p->attr_name_len = 0;
    p->attr_value_len = 0;
}

/* Resolves `raw` against the parser's base URL when it has one; with no base
 * URL, `raw` is reported exactly as written (see core_sdk/html.h). Returns
 * NULL when there is a base URL but resolution against it fails (e.g. a
 * malformed relative reference), so the caller should drop the event. */
static const char *html__resolve_or_pass_through(bruce_html_parser_t *p, const char *raw) {
    if (!p->has_base_url) return raw;
    if (html__resolve_url(p->base_url, raw, p->resolved_url, sizeof(p->resolved_url))) return p->resolved_url;
    return NULL;
}

static void html__emit_link_start(bruce_html_parser_t *p) {
    if (!p->has_href) return;
    const char *url = html__resolve_or_pass_through(p, p->href_value);
    if (url == NULL) return;
    html__flush_text(p);
    bruce_html_event_t event = {0};
    event.type = BRUCE_HTML_EVENT_LINK_START;
    event.text = url;
    event.text_len = strlen(url);
    html__emit(p, &event);
    p->in_link = true;
}

static void html__emit_image(bruce_html_parser_t *p) {
    if (!p->has_src) return;
    const char *url = html__resolve_or_pass_through(p, p->src_value);
    if (url == NULL) return;
    html__flush_text(p);
    bruce_html_event_t event = {0};
    event.type = BRUCE_HTML_EVENT_IMAGE;
    event.text = url;
    event.text_len = strlen(url);
    if (p->has_alt) {
        event.alt = p->alt_value;
        event.alt_len = p->alt_len;
    }
    html__emit(p, &event);
}

static bool html__is_heading(html_tag_id_t tag, int *out_level) {
    switch (tag) {
    case HTML_TAG_H1: *out_level = 1; return true;
    case HTML_TAG_H2: *out_level = 2; return true;
    case HTML_TAG_H3: *out_level = 3; return true;
    case HTML_TAG_H4: *out_level = 4; return true;
    case HTML_TAG_H5: *out_level = 5; return true;
    case HTML_TAG_H6: *out_level = 6; return true;
    default: return false;
    }
}

static const char *html__rawtext_name_for(html_tag_id_t tag, size_t *out_len) {
    if (tag == HTML_TAG_SCRIPT) {
        *out_len = 6;
        return "script";
    }
    if (tag == HTML_TAG_STYLE) {
        *out_len = 5;
        return "style";
    }
    return NULL;
}

html_parser_state_t html__handle_tag_end(bruce_html_parser_t *p) {
    html_parser_state_t next = HTML_PS_TEXT;
    int heading_level = 0;

    if (!p->tag_is_closing) {
        if (p->current_tag == HTML_TAG_A) {
            html__emit_link_start(p);
        } else if (p->current_tag == HTML_TAG_IMG) {
            html__emit_image(p);
        } else if (p->current_tag == HTML_TAG_BR) {
            html__break(p, BRUCE_HTML_EVENT_LINE_BREAK);
        } else if (p->current_tag == HTML_TAG_HR) {
            html__break(p, BRUCE_HTML_EVENT_PARAGRAPH_BREAK);
        } else if (p->current_tag == HTML_TAG_TITLE) {
            p->in_title = true;
            p->title_len = 0;
            p->last_was_space = true;
        } else if (html__is_heading(p->current_tag, &heading_level)) {
            p->heading_level = heading_level;
            html__flush_text(p);
            bruce_html_event_t event = {0};
            event.type = BRUCE_HTML_EVENT_HEADING_START;
            event.value = heading_level;
            html__emit(p, &event);
        } else {
            size_t rawtext_len = 0;
            const char *rawtext_name = html__rawtext_name_for(p->current_tag, &rawtext_len);
            if (rawtext_name != NULL) {
                next = HTML_PS_RAWTEXT;
                p->rawtext_end_tag = rawtext_name;
                p->rawtext_end_tag_len = rawtext_len;
                p->rawtext_match_pos = 0;
                p->rawtext_closing = false;
            }
        }
    } else {
        if (p->current_tag == HTML_TAG_A) {
            if (p->in_link) {
                html__flush_text(p);
                bruce_html_event_t event = {0};
                event.type = BRUCE_HTML_EVENT_LINK_END;
                html__emit(p, &event);
                p->in_link = false;
            }
        } else if (p->current_tag == HTML_TAG_TITLE) {
            if (p->in_title) {
                bruce_html_event_t event = {0};
                event.type = BRUCE_HTML_EVENT_TITLE;
                event.text = p->title_buffer;
                event.text_len = p->title_len;
                html__emit(p, &event);
                p->in_title = false;
                p->last_was_space = true;
            }
        } else if (html__is_heading(p->current_tag, &heading_level)) {
            html__flush_text(p);
            bruce_html_event_t event = {0};
            event.type = BRUCE_HTML_EVENT_HEADING_END;
            html__emit(p, &event);
            p->heading_level = 0;
            html__break(p, BRUCE_HTML_EVENT_PARAGRAPH_BREAK);
        } else if (p->current_tag == HTML_TAG_BLOCK) {
            html__break(p, BRUCE_HTML_EVENT_PARAGRAPH_BREAK);
        }
    }

    p->has_href = false;
    p->has_src = false;
    p->has_alt = false;
    p->href_len = 0;
    p->src_len = 0;
    p->alt_len = 0;
    return next;
}

