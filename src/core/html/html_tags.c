#include <string.h>

#include "html_internal.h"

/*
 * The fixed vocabulary of tags core_sdk/html.h's parser gives special
 * meaning to. Anything not listed here is HTML_TAG_UNKNOWN and is treated as
 * transparent by html_parser.c/html_events.c: the tag disappears and its
 * content flows into the surrounding text, which is the whole point -- this
 * table is deliberately short rather than an exhaustive HTML5 tag list.
 */

typedef struct {
    const char *name;
    html_tag_id_t id;
} html_tag_entry_t;

static const html_tag_entry_t HTML_TAGS[] = {
    {"a",          HTML_TAG_A         },
    {"img",        HTML_TAG_IMG       },
    {"br",         HTML_TAG_BR        },
    {"hr",         HTML_TAG_HR        },
    {"title",      HTML_TAG_TITLE     },
    {"script",     HTML_TAG_SCRIPT    },
    {"style",      HTML_TAG_STYLE     },
    {"h1",         HTML_TAG_H1        },
    {"h2",         HTML_TAG_H2        },
    {"h3",         HTML_TAG_H3        },
    {"h4",         HTML_TAG_H4        },
    {"h5",         HTML_TAG_H5        },
    {"h6",         HTML_TAG_H6        },
    {"p",          HTML_TAG_BLOCK     },
    {"div",        HTML_TAG_BLOCK     },
    {"li",         HTML_TAG_LIST_ITEM },
    {"tr",         HTML_TAG_BLOCK     },
    {"table",      HTML_TAG_BLOCK     },
    {"ul",         HTML_TAG_LIST      },
    {"ol",         HTML_TAG_LIST      },
    {"blockquote", HTML_TAG_BLOCK     },
    {"header",     HTML_TAG_BLOCK     },
    {"footer",     HTML_TAG_BLOCK     },
    {"section",    HTML_TAG_BLOCK     },
    {"main",       HTML_TAG_MAIN      },
    {"article",    HTML_TAG_ARTICLE   },
    {"nav",        HTML_TAG_NAV       },
    {"dl",         HTML_TAG_BLOCK     },
    {"dt",         HTML_TAG_BLOCK     },
    {"dd",         HTML_TAG_BLOCK     },
    {"form",       HTML_TAG_BLOCK     },
    {"td",         HTML_TAG_BLOCK     },
    {"th",         HTML_TAG_BLOCK     },
};

html_tag_id_t html__lookup_tag(const char *name, size_t len) {
    for (size_t i = 0; i < sizeof(HTML_TAGS) / sizeof(HTML_TAGS[0]); ++i) {
        if (strlen(HTML_TAGS[i].name) == len && memcmp(HTML_TAGS[i].name, name, len) == 0) {
            return HTML_TAGS[i].id;
        }
    }
    return HTML_TAG_UNKNOWN;
}
