#include <ctype.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "html_internal.h"

/*
 * Streaming HTML tokenizer: the byte-level state machine and its text/entity
 * buffers. See html_tags.c for the fixed tag vocabulary it recognizes and
 * html_events.c for what a completed tag means (html__handle_tag_end(), the
 * hand-off point from here). An unrecognized tag such as <span> or <b> is
 * transparent -- it disappears and its content flows into the surrounding
 * text, which is exactly the "extract only what I care about" behavior
 * core_sdk/html.h documents. <title> is parsed with the normal text
 * tokenizer rather than as HTML5's raw RCDATA element, so a literal '<'
 * inside a page title (essentially never seen in practice) would be
 * misread as a tag.
 *
 * Bytes are consumed one at a time by html__feed_byte(). A handful of state
 * transitions re-dispatch the same byte into the new state (e.g. "<x" where
 * 'x' turns out not to start a valid tag name) by calling html__feed_byte()
 * again; every such call site changes state first, so recursion is always
 * shallow and terminates.
 */

static bool html__is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static void html__feed_byte(bruce_html_parser_t *p, unsigned char c);

void html__emit(bruce_html_parser_t *p, const bruce_html_event_t *event) { p->callback(event, p->context); }

void html__flush_text(bruce_html_parser_t *p) {
    if (p->text_len == 0) return;
    bruce_html_event_t event = {0};
    event.type = BRUCE_HTML_EVENT_TEXT;
    event.text = p->text_buffer;
    event.text_len = p->text_len;
    html__emit(p, &event);
    p->text_len = 0;
}

void html__break(bruce_html_parser_t *p, bruce_html_event_type_t type) {
    html__flush_text(p);
    bruce_html_event_t event = {0};
    event.type = type;
    html__emit(p, &event);
    p->last_was_space = true;
}

static void html__emit_char(bruce_html_parser_t *p, char c) {
    if (html__is_space(c)) {
        if (p->last_was_space) return;
        c = ' ';
        p->last_was_space = true;
    } else {
        p->last_was_space = false;
    }
    if (p->in_title) {
        if (p->title_len < sizeof(p->title_buffer)) p->title_buffer[p->title_len++] = c;
        return;
    }
    if (p->text_len >= sizeof(p->text_buffer)) html__flush_text(p);
    p->text_buffer[p->text_len++] = c;
}

static void html__append_attr_char(bruce_html_parser_t *p, char c) {
    if (p->attr_value_len < sizeof(p->attr_value) - 1u) p->attr_value[p->attr_value_len++] = c;
}

static void html__entity_output(bruce_html_parser_t *p, char c) {
    if (p->entity_return_state == HTML_PS_TEXT) html__emit_char(p, c);
    else html__append_attr_char(p, c);
}

static void html__start_entity(bruce_html_parser_t *p, html_parser_state_t return_state) {
    p->entity_return_state = return_state;
    p->entity_len = 0;
    p->state = HTML_PS_ENTITY;
}

static void html__feed_byte(bruce_html_parser_t *p, unsigned char c) {
    switch (p->state) {
    case HTML_PS_TEXT:
        if (c == '<') p->state = HTML_PS_TAG_OPEN;
        else if (c == '&') html__start_entity(p, HTML_PS_TEXT);
        else html__emit_char(p, (char)c);
        break;

    case HTML_PS_TAG_OPEN:
        if (c == '!') {
            p->state = HTML_PS_TAG_BANG;
        } else if (c == '/') {
            p->tag_is_closing = true;
            p->tag_name_len = 0;
            p->state = HTML_PS_END_TAG_OPEN;
        } else if (c == '?') {
            p->state = HTML_PS_MARKUP_DECL_SKIP;
        } else if (isalpha(c)) {
            p->tag_is_closing = false;
            p->tag_name_len = 0;
            p->tag_name[p->tag_name_len++] = (char)tolower(c);
            p->state = HTML_PS_TAG_NAME;
        } else {
            p->state = HTML_PS_TEXT;
            html__emit_char(p, '<');
            html__feed_byte(p, c);
        }
        break;

    case HTML_PS_END_TAG_OPEN:
        if (isalpha(c)) {
            p->tag_name[p->tag_name_len++] = (char)tolower(c);
            p->state = HTML_PS_TAG_NAME;
        } else {
            p->state = HTML_PS_MARKUP_DECL_SKIP;
            html__feed_byte(p, c);
        }
        break;

    case HTML_PS_TAG_NAME:
        if (isalnum(c) || c == '-') {
            if (p->tag_name_len < sizeof(p->tag_name)) p->tag_name[p->tag_name_len++] = (char)tolower(c);
        } else {
            p->current_tag = html__lookup_tag(p->tag_name, p->tag_name_len);
            p->state = HTML_PS_BEFORE_ATTR_NAME;
            html__feed_byte(p, c);
        }
        break;

    case HTML_PS_BEFORE_ATTR_NAME:
        if (c == '>') {
            p->state = html__handle_tag_end(p);
        } else if (c == '/' || html__is_space((char)c)) {
            /* skip */
        } else if (isalpha(c)) {
            p->attr_name_len = 0;
            p->attr_name[p->attr_name_len++] = (char)tolower(c);
            p->state = HTML_PS_ATTR_NAME;
        }
        break;

    case HTML_PS_ATTR_NAME:
        if (html__is_space((char)c)) {
            p->state = HTML_PS_AFTER_ATTR_NAME;
        } else if (c == '=') {
            p->state = HTML_PS_BEFORE_ATTR_VALUE;
        } else if (c == '>') {
            p->attr_name_len = 0;
            p->state = html__handle_tag_end(p);
        } else if (c == '/') {
            p->attr_name_len = 0;
            p->state = HTML_PS_BEFORE_ATTR_NAME;
        } else if (p->attr_name_len < sizeof(p->attr_name)) {
            p->attr_name[p->attr_name_len++] = (char)tolower(c);
        }
        break;

    case HTML_PS_AFTER_ATTR_NAME:
        if (html__is_space((char)c)) {
            /* skip */
        } else if (c == '=') {
            p->state = HTML_PS_BEFORE_ATTR_VALUE;
        } else if (c == '>') {
            p->attr_name_len = 0;
            p->state = html__handle_tag_end(p);
        } else if (c == '/') {
            p->attr_name_len = 0;
            p->state = HTML_PS_BEFORE_ATTR_NAME;
        } else {
            p->attr_name_len = 0;
            p->state = HTML_PS_BEFORE_ATTR_NAME;
            html__feed_byte(p, c);
        }
        break;

    case HTML_PS_BEFORE_ATTR_VALUE:
        if (html__is_space((char)c)) {
            /* skip */
        } else if (c == '"' || c == '\'') {
            p->quote_char = (char)c;
            p->attr_value_len = 0;
            p->state = HTML_PS_ATTR_VALUE_QUOTED;
        } else if (c == '>') {
            p->attr_name_len = 0;
            p->attr_value_len = 0;
            p->state = html__handle_tag_end(p);
        } else {
            p->attr_value_len = 0;
            p->state = HTML_PS_ATTR_VALUE_UNQUOTED;
            html__feed_byte(p, c);
        }
        break;

    case HTML_PS_ATTR_VALUE_QUOTED:
        if (c == (unsigned char)p->quote_char) {
            html__finalize_attr(p);
            p->state = HTML_PS_BEFORE_ATTR_NAME;
        } else if (c == '&') {
            html__start_entity(p, HTML_PS_ATTR_VALUE_QUOTED);
        } else {
            html__append_attr_char(p, (char)c);
        }
        break;

    case HTML_PS_ATTR_VALUE_UNQUOTED:
        if (html__is_space((char)c) || c == '>') {
            html__finalize_attr(p);
            p->state = HTML_PS_BEFORE_ATTR_NAME;
            html__feed_byte(p, c);
        } else if (c == '&') {
            html__start_entity(p, HTML_PS_ATTR_VALUE_UNQUOTED);
        } else {
            html__append_attr_char(p, (char)c);
        }
        break;

    case HTML_PS_TAG_BANG:
        if (c == '-') p->state = HTML_PS_TAG_BANG_DASH;
        else {
            p->state = HTML_PS_MARKUP_DECL_SKIP;
            html__feed_byte(p, c);
        }
        break;

    case HTML_PS_TAG_BANG_DASH:
        if (c == '-') {
            p->state = HTML_PS_COMMENT;
            p->comment_dash_count = 0;
        } else {
            p->state = HTML_PS_MARKUP_DECL_SKIP;
            html__feed_byte(p, c);
        }
        break;

    case HTML_PS_MARKUP_DECL_SKIP:
        if (c == '>') p->state = HTML_PS_TEXT;
        break;

    case HTML_PS_COMMENT:
        if (c == '-') {
            if (p->comment_dash_count < 2) p->comment_dash_count++;
        } else if (c == '>' && p->comment_dash_count >= 2) {
            p->state = HTML_PS_TEXT;
            p->comment_dash_count = 0;
        } else {
            p->comment_dash_count = 0;
        }
        break;

    case HTML_PS_RAWTEXT: {
        char lc = (char)tolower(c);
        if (p->rawtext_closing) {
            if (c == '>') {
                p->state = HTML_PS_TEXT;
                p->rawtext_closing = false;
            }
        } else if (p->rawtext_match_pos == 0 && c == '<') {
            p->rawtext_match_pos = 1;
        } else if (p->rawtext_match_pos == 1 && c == '/') {
            p->rawtext_match_pos = 2;
        } else if (p->rawtext_match_pos >= 2 &&
                   (size_t)(p->rawtext_match_pos - 2) < p->rawtext_end_tag_len &&
                   lc == p->rawtext_end_tag[p->rawtext_match_pos - 2]) {
            p->rawtext_match_pos++;
            if ((size_t)(p->rawtext_match_pos - 2) == p->rawtext_end_tag_len) p->rawtext_closing = true;
        } else {
            p->rawtext_match_pos = (c == '<') ? 1u : 0u;
        }
        break;
    }

    case HTML_PS_ENTITY: {
        if (c == ';') {
            char decoded[8];
            size_t decoded_len = 0;
            if (html__decode_entity(p->entity_buffer, p->entity_len, decoded, sizeof(decoded), &decoded_len)) {
                for (size_t i = 0; i < decoded_len; ++i) html__entity_output(p, decoded[i]);
            } else {
                html__entity_output(p, '&');
                for (size_t i = 0; i < p->entity_len; ++i) html__entity_output(p, p->entity_buffer[i]);
                html__entity_output(p, ';');
            }
            p->state = p->entity_return_state;
            p->entity_len = 0;
            break;
        }
        bool is_delim = c == '<' || c == '&' || html__is_space((char)c) ||
                        (p->entity_return_state == HTML_PS_ATTR_VALUE_QUOTED && c == (unsigned char)p->quote_char) ||
                        (p->entity_return_state == HTML_PS_ATTR_VALUE_UNQUOTED && c == '>');
        if (is_delim || p->entity_len >= sizeof(p->entity_buffer)) {
            html__entity_output(p, '&');
            for (size_t i = 0; i < p->entity_len; ++i) html__entity_output(p, p->entity_buffer[i]);
            p->state = p->entity_return_state;
            p->entity_len = 0;
            html__feed_byte(p, c);
        } else {
            p->entity_buffer[p->entity_len++] = (char)c;
        }
        break;
    }
    }
}

bruce_result_t html__parser_create(
    const char *base_url, bruce_html_event_cb_t callback, void *context, bruce_html_parser_t **out_parser
) {
    if (callback == NULL || out_parser == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_html_parser_t *p = memory__malloc(sizeof(*p));
    if (p == NULL) return BRUCE_ERR_NO_MEMORY;
    memset(p, 0, sizeof(*p));
    p->callback = callback;
    p->context = context;
    p->state = HTML_PS_TEXT;
    p->last_was_space = true;
    if (base_url != NULL && base_url[0] != '\0') {
        size_t n = strlen(base_url);
        if (n >= sizeof(p->base_url)) n = sizeof(p->base_url) - 1u;
        memcpy(p->base_url, base_url, n);
        p->base_url[n] = '\0';
        p->has_base_url = true;
    }
    *out_parser = p;
    return BRUCE_OK;
}

bruce_result_t html__parser_feed(bruce_html_parser_t *parser, const void *data, size_t len) {
    if (parser == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (data == NULL || len == 0) return BRUCE_OK;
    const unsigned char *bytes = data;
    for (size_t i = 0; i < len; ++i) html__feed_byte(parser, bytes[i]);
    return BRUCE_OK;
}

bruce_result_t html__parser_finish(bruce_html_parser_t *parser) {
    if (parser == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (parser->state == HTML_PS_ENTITY) {
        /* Document ended mid-entity (e.g. a trailing "&amp" with no ';'):
         * emit it literally instead of silently dropping it. */
        html__entity_output(parser, '&');
        for (size_t i = 0; i < parser->entity_len; ++i) html__entity_output(parser, parser->entity_buffer[i]);
        parser->entity_len = 0;
        parser->state = parser->entity_return_state;
    }
    html__flush_text(parser);
    return BRUCE_OK;
}

void html__parser_destroy(bruce_html_parser_t *parser) { memory__free(parser); }
