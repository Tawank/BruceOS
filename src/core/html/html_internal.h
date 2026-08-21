#pragma once

/*
 * Private state for the streaming HTML parser (core_sdk/html.h). See
 * html_parser.c for the tokenizer, html_entities.c for entity decoding, and
 * html_url.c for the public html__resolve_url() implementation shared by all
 * three.
 */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/html.h"

/* Generous enough for real-world URLs/attribute values without costing much
 * per parser instance; longer values are truncated rather than rejected. */
#define HTML_URL_MAX 400
#define HTML_ATTR_VALUE_MAX HTML_URL_MAX
#define HTML_TAG_NAME_MAX 16
#define HTML_ATTR_NAME_MAX 24
#define HTML_ENTITY_NAME_MAX 16
#define HTML_TEXT_CHUNK_MAX 128
#define HTML_TITLE_MAX 128

typedef enum {
    HTML_TAG_UNKNOWN = 0,
    HTML_TAG_A,
    HTML_TAG_IMG,
    HTML_TAG_BR,
    HTML_TAG_HR,
    HTML_TAG_TITLE,
    HTML_TAG_SCRIPT,
    HTML_TAG_STYLE,
    HTML_TAG_H1,
    HTML_TAG_H2,
    HTML_TAG_H3,
    HTML_TAG_H4,
    HTML_TAG_H5,
    HTML_TAG_H6,
    /* Block-level tags whose *close* just breaks the text flow: p, div, li,
     * tr, table, ul, ol, blockquote, header, footer, section, dl, dt, dd,
     * form, td, th. */
    HTML_TAG_BLOCK,
    /* Landmarks: block-level on close like HTML_TAG_BLOCK above, but their
     * *open* also reports a BRUCE_HTML_EVENT_LANDMARK_START (see
     * html__handle_tag_end() in html_events.c). */
    HTML_TAG_MAIN,
    HTML_TAG_ARTICLE,
    HTML_TAG_NAV,
} html_tag_id_t;

typedef enum {
    HTML_PS_TEXT = 0,
    HTML_PS_TAG_OPEN,
    HTML_PS_END_TAG_OPEN,
    HTML_PS_TAG_NAME,
    HTML_PS_BEFORE_ATTR_NAME,
    HTML_PS_ATTR_NAME,
    HTML_PS_AFTER_ATTR_NAME,
    HTML_PS_BEFORE_ATTR_VALUE,
    HTML_PS_ATTR_VALUE_QUOTED,
    HTML_PS_ATTR_VALUE_UNQUOTED,
    HTML_PS_TAG_BANG,
    HTML_PS_TAG_BANG_DASH,
    HTML_PS_COMMENT,
    HTML_PS_MARKUP_DECL_SKIP,
    HTML_PS_RAWTEXT,
    HTML_PS_ENTITY,
} html_parser_state_t;

struct bruce_html_parser {
    bruce_html_event_cb_t callback;
    void *context;

    char base_url[HTML_URL_MAX];
    bool has_base_url;
    /* Scratch buffer reused for each resolved href/src, kept on the parser
     * instance instead of a call-stack local. */
    char resolved_url[HTML_URL_MAX];

    html_parser_state_t state;

    char tag_name[HTML_TAG_NAME_MAX];
    size_t tag_name_len;
    bool tag_is_closing;
    html_tag_id_t current_tag;

    char attr_name[HTML_ATTR_NAME_MAX];
    size_t attr_name_len;
    char attr_value[HTML_ATTR_VALUE_MAX];
    size_t attr_value_len;
    char quote_char;

    char href_value[HTML_ATTR_VALUE_MAX];
    size_t href_len;
    bool has_href;
    char src_value[HTML_ATTR_VALUE_MAX];
    size_t src_len;
    bool has_src;
    char alt_value[HTML_ATTR_VALUE_MAX];
    size_t alt_len;
    bool has_alt;
    char id_value[HTML_ATTR_VALUE_MAX];
    size_t id_len;
    bool has_id;

    /* <script>/<style> raw-content skip: incremental case-insensitive match
     * against "</" + rawtext_end_tag. */
    const char *rawtext_end_tag;
    size_t rawtext_end_tag_len;
    size_t rawtext_match_pos;
    bool rawtext_closing;

    int comment_dash_count;

    char text_buffer[HTML_TEXT_CHUNK_MAX];
    size_t text_len;
    bool last_was_space;

    char title_buffer[HTML_TITLE_MAX];
    size_t title_len;
    bool in_title;

    bool in_link;
    int heading_level;

    char entity_buffer[HTML_ENTITY_NAME_MAX];
    size_t entity_len;
    html_parser_state_t entity_return_state;
};

/* Looks up a lowercased tag name; unrecognized names return HTML_TAG_UNKNOWN.
 * Implemented in html_tags.c. */
html_tag_id_t html__lookup_tag(const char *name, size_t len);

/* Decodes one entity name (the text between '&' and ';', not including
 * either) into `out`. Returns true and fills `*out_len` on success (a known
 * named entity such as "amp", or a numeric reference such as "#39"/"#x27");
 * returns false for anything unrecognized, leaving `*out_len` untouched.
 * Implemented in html_entities.c. */
bool html__decode_entity(const char *name, size_t name_len, char *out, size_t out_capacity, size_t *out_len);

/*
 * The pieces below are shared between html_parser.c (the byte-level
 * tokenizer state machine) and html_events.c (what a completed tag means:
 * resolving/reporting links and images, headings, block breaks) -- kept
 * non-static so the tokenizer can hand off to html__handle_tag_end() and the
 * event layer can flush/emit through the same text buffer.
 */

/* Emits `*event`, filling in nothing else -- callers set every field they
 * care about first. */
void html__emit(bruce_html_parser_t *p, const bruce_html_event_t *event);

/* Emits the pending text buffer as one BRUCE_HTML_EVENT_TEXT event, if
 * non-empty. */
void html__flush_text(bruce_html_parser_t *p);

/* Flushes pending text, emits a structural break, and resets whitespace
 * collapsing so the next text run doesn't start with a stray leading space. */
void html__break(bruce_html_parser_t *p, bruce_html_event_type_t type);

/* Called when '>' closes a tag (p->tag_is_closing/current_tag already set).
 * Returns the state to resume in: HTML_PS_TEXT, unless the tag opens a
 * <script>/<style> raw-content block, and clears the pending
 * href/src/alt attribute capture either way. Implemented in html_events.c. */
html_parser_state_t html__handle_tag_end(bruce_html_parser_t *p);

/* Matches p->attr_name against href/src/alt, copying p->attr_value into the
 * matching destination field (if any) and resetting both scratch buffers.
 * Implemented in html_events.c. */
void html__finalize_attr(bruce_html_parser_t *p);
