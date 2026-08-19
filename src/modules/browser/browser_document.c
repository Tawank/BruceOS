#include "browser_document.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"

#define BROWSER_INITIAL_TEXT_CAP 1024u
#define BROWSER_INITIAL_ITEM_CAP 64u
#define BROWSER_INITIAL_LINK_CAP 16u
#define BROWSER_INITIAL_IMAGE_CAP 8u

bruce_result_t browser_document__create(browser_document_t **out_doc) {
    if (out_doc == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    browser_document_t *doc = memory__malloc(sizeof(*doc));
    if (doc == NULL) return BRUCE_ERR_NO_MEMORY;
    memset(doc, 0, sizeof(*doc));

    doc->text_pool = memory__malloc(BROWSER_INITIAL_TEXT_CAP);
    doc->items = memory__malloc(BROWSER_INITIAL_ITEM_CAP * sizeof(*doc->items));
    doc->links = memory__malloc(BROWSER_INITIAL_LINK_CAP * sizeof(*doc->links));
    doc->images = memory__malloc(BROWSER_INITIAL_IMAGE_CAP * sizeof(*doc->images));
    if (doc->text_pool == NULL || doc->items == NULL || doc->links == NULL || doc->images == NULL) {
        browser_document__destroy(doc);
        return BRUCE_ERR_NO_MEMORY;
    }
    doc->text_pool_cap = BROWSER_INITIAL_TEXT_CAP;
    doc->item_cap = BROWSER_INITIAL_ITEM_CAP;
    doc->link_cap = BROWSER_INITIAL_LINK_CAP;
    doc->image_cap = BROWSER_INITIAL_IMAGE_CAP;
    *out_doc = doc;
    return BRUCE_OK;
}

void browser_document__destroy(browser_document_t *doc) {
    if (doc == NULL) return;
    memory__free(doc->text_pool);
    memory__free(doc->items);
    memory__free(doc->links);
    memory__free(doc->images);
    memory__free(doc);
}

void browser_document__reset(browser_document_t *doc) {
    if (doc == NULL) return;
    doc->text_pool_len = 0;
    doc->item_count = 0;
    doc->link_count = 0;
    doc->image_count = 0;
    doc->title[0] = '\0';
    doc->url[0] = '\0';
    doc->truncated = false;
}

void browser_document__set_url(browser_document_t *doc, const char *url) {
    if (doc == NULL || url == NULL) return;
    snprintf(doc->url, sizeof(doc->url), "%s", url);
}

void browser_document__set_title(browser_document_t *doc, const char *title, size_t len) {
    if (doc == NULL || title == NULL) return;
    if (len >= sizeof(doc->title)) len = sizeof(doc->title) - 1u;
    memcpy(doc->title, title, len);
    doc->title[len] = '\0';
}

/* Doubles `*cap` (up to `hard_max`) and grows `*array` (whose elements are
 * `elem_size` bytes) to match. Returns false, leaving everything unchanged,
 * when already at `hard_max` or on allocation failure. */
static bool browser_document__grow(void **array, size_t *cap, size_t elem_size, size_t hard_max) {
    if (*cap >= hard_max) return false;
    size_t next_cap = *cap * 2u;
    if (next_cap > hard_max) next_cap = hard_max;
    void *grown = memory__realloc(*array, next_cap * elem_size);
    if (grown == NULL) return false;
    *array = grown;
    *cap = next_cap;
    return true;
}

static browser_item_t *browser_document__new_item(browser_document_t *doc) {
    if (doc->item_count >= doc->item_cap &&
        !browser_document__grow((void **)&doc->items, &doc->item_cap, sizeof(*doc->items), BROWSER_MAX_ITEMS)) {
        doc->truncated = true;
        return NULL;
    }
    if (doc->item_count >= doc->item_cap) {
        doc->truncated = true;
        return NULL;
    }
    browser_item_t *item = &doc->items[doc->item_count++];
    *item = (browser_item_t){.kind = BROWSER_ITEM_TEXT, .link_index = -1, .image_index = -1};
    return item;
}

void browser_document__add_text(
    browser_document_t *doc, const char *text, size_t len, int heading_level, int link_index
) {
    if (doc == NULL || text == NULL || len == 0) return;
    while (doc->text_pool_len + len > doc->text_pool_cap) {
        if (!browser_document__grow(
                (void **)&doc->text_pool, &doc->text_pool_cap, 1, BROWSER_MAX_TEXT_BYTES
            )) {
            doc->truncated = true;
            /* Fit whatever still fits rather than dropping the whole run. */
            len = doc->text_pool_len < doc->text_pool_cap ? doc->text_pool_cap - doc->text_pool_len : 0;
            if (len == 0) return;
            break;
        }
    }
    browser_item_t *item = browser_document__new_item(doc);
    if (item == NULL) return;
    memcpy(doc->text_pool + doc->text_pool_len, text, len);
    item->kind = BROWSER_ITEM_TEXT;
    item->text_offset = doc->text_pool_len;
    item->text_len = len;
    item->heading_level = heading_level;
    item->link_index = link_index;
    doc->text_pool_len += len;
}

int browser_document__add_link(browser_document_t *doc, const char *url) {
    if (doc == NULL || url == NULL) return -1;
    if (doc->link_count >= doc->link_cap &&
        !browser_document__grow((void **)&doc->links, &doc->link_cap, sizeof(*doc->links), BROWSER_MAX_LINKS)) {
        doc->truncated = true;
        return -1;
    }
    if (doc->link_count >= doc->link_cap) {
        doc->truncated = true;
        return -1;
    }
    snprintf(doc->links[doc->link_count].url, sizeof(doc->links[0].url), "%s", url);
    return (int)doc->link_count++;
}

void browser_document__add_image(browser_document_t *doc, const char *url, const char *alt, size_t alt_len) {
    if (doc == NULL || url == NULL) return;
    if (doc->image_count >= doc->image_cap &&
        !browser_document__grow((void **)&doc->images, &doc->image_cap, sizeof(*doc->images), BROWSER_MAX_IMAGES)) {
        doc->truncated = true;
        return;
    }
    if (doc->image_count >= doc->image_cap) {
        doc->truncated = true;
        return;
    }
    browser_item_t *item = browser_document__new_item(doc);
    if (item == NULL) return;
    browser_image_ref_t *image = &doc->images[doc->image_count];
    snprintf(image->url, sizeof(image->url), "%s", url);
    if (alt != NULL && alt_len > 0) {
        if (alt_len >= sizeof(image->alt)) alt_len = sizeof(image->alt) - 1u;
        memcpy(image->alt, alt, alt_len);
        image->alt[alt_len] = '\0';
    } else {
        image->alt[0] = '\0';
    }
    item->kind = BROWSER_ITEM_IMAGE;
    item->image_index = (int)doc->image_count++;
}

void browser_document__add_break(browser_document_t *doc, bool paragraph) {
    if (doc == NULL) return;
    browser_item_t *item = browser_document__new_item(doc);
    if (item == NULL) return;
    item->kind = paragraph ? BROWSER_ITEM_PARAGRAPH_BREAK : BROWSER_ITEM_LINE_BREAK;
}
