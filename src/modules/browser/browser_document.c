#include "browser_document.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"

#define BROWSER_INITIAL_TEXT_CAP 1024u
#define BROWSER_INITIAL_ITEM_CAP 64u
#define BROWSER_INITIAL_LINK_CAP 16u
#define BROWSER_INITIAL_IMAGE_CAP 8u
#define BROWSER_INITIAL_ANCHOR_CAP 8u

bruce_result_t browser_document__create(browser_document_t **out_doc) {
    if (out_doc == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    browser_document_t *doc = memory__malloc(sizeof(*doc));
    if (doc == NULL) return BRUCE_ERR_NO_MEMORY;
    /* Zeroing leaves text_pool_object/links_object at BRUCE_MEMORY_BACKEND_INVALID
     * (0) -- both are allocated lazily, on the first add_text()/add_link()
     * call, rather than up front here, so a page with no text or no links
     * never touches the external allocator at all. */
    memset(doc, 0, sizeof(*doc));

    doc->items = memory__malloc(BROWSER_INITIAL_ITEM_CAP * sizeof(*doc->items));
    if (doc->items == NULL) {
        browser_document__destroy(doc);
        return BRUCE_ERR_NO_MEMORY;
    }
    doc->item_cap = BROWSER_INITIAL_ITEM_CAP;
    doc->main_item_index = -1;
    doc->article_item_index = -1;
    doc->nav_item_index = -1;
    *out_doc = doc;
    return BRUCE_OK;
}

void browser_document__destroy(browser_document_t *doc) {
    if (doc == NULL) return;
    if (doc->text_pool_object.backend != BRUCE_MEMORY_BACKEND_INVALID) {
        (void)memory__external_free(&doc->text_pool_object);
    }
    if (doc->links_object.backend != BRUCE_MEMORY_BACKEND_INVALID) {
        (void)memory__external_free(&doc->links_object);
    }
    if (doc->images_object.backend != BRUCE_MEMORY_BACKEND_INVALID) {
        (void)memory__external_free(&doc->images_object);
    }
    if (doc->anchors_object.backend != BRUCE_MEMORY_BACKEND_INVALID) {
        (void)memory__external_free(&doc->anchors_object);
    }
    memory__free(doc->items);
    memory__free(doc);
}

/* Grows an external (PSRAM/swap, or internal RAM only as a last resort --
 * see memory__external_alloc()) buffer to hold at least `used + extra`
 * bytes, migrating the `used` bytes already written across, and updates
 * `*mapped` to the freshly mapped read pointer. External objects can't be
 * resized in place, so -- exactly like core/http/http.c's own
 * http__external_body_reserve() -- each growth step allocates a new, bigger
 * object and copies the old one across via a read-only map plus a write.
 * A no-op returning true when the existing object already fits. */
static bool browser_document__ext_reserve(
    bruce_memory_object_t *object, const void **mapped, size_t used, size_t extra, size_t initial, size_t hard_max
) {
    size_t capacity = object->backend != BRUCE_MEMORY_BACKEND_INVALID ? object->size : 0;
    size_t required = used + extra;
    if (required <= capacity) return true;
    if (required > hard_max) return false;

    size_t new_capacity = capacity == 0 ? initial : capacity;
    while (new_capacity < required) {
        new_capacity = new_capacity > hard_max / 2 ? hard_max : new_capacity * 2;
    }
    bruce_memory_object_t grown;
    if (memory__external_alloc(new_capacity, &grown) != BRUCE_OK) return false;
    if (used > 0 && memory__external_write(&grown, 0, *mapped, used) != BRUCE_OK) {
        (void)memory__external_free(&grown);
        return false;
    }
    const void *new_map = NULL;
    if (memory__external_map(&grown, &new_map) != BRUCE_OK) {
        (void)memory__external_free(&grown);
        return false;
    }
    if (object->backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(object);
    *object = grown;
    *mapped = new_map;
    return true;
}

void browser_document__reset(browser_document_t *doc) {
    if (doc == NULL) return;
    doc->text_pool_len = 0;
    doc->item_count = 0;
    doc->link_count = 0;
    doc->image_count = 0;
    doc->anchor_count = 0;
    doc->main_item_index = -1;
    doc->article_item_index = -1;
    doc->nav_item_index = -1;
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
    *item = (browser_item_t){.kind = (uint8_t)BROWSER_ITEM_TEXT, .link_index = -1, .image_index = -1};
    return item;
}

void browser_document__shrink_to_fit(browser_document_t *doc) {
    if (doc == NULL || doc->item_count == 0 || doc->item_count >= doc->item_cap) return;
    browser_item_t *shrunk = memory__realloc(doc->items, doc->item_count * sizeof(*doc->items));
    if (shrunk == NULL) return; /* Shrinking realloc failing is harmless; keep the larger buffer. */
    doc->items = shrunk;
    doc->item_cap = doc->item_count;
}

void browser_document__add_text(
    browser_document_t *doc, const char *text, size_t len, int heading_level, int link_index
) {
    if (doc == NULL || text == NULL || len == 0) return;
    if (!browser_document__ext_reserve(
            &doc->text_pool_object, (const void **)&doc->text_pool, doc->text_pool_len, len,
            BROWSER_INITIAL_TEXT_CAP, BROWSER_MAX_TEXT_BYTES
        )) {
        doc->truncated = true;
        /* Fit whatever still fits rather than dropping the whole run. */
        size_t capacity = doc->text_pool_object.backend != BRUCE_MEMORY_BACKEND_INVALID
                               ? doc->text_pool_object.size
                               : 0;
        len = doc->text_pool_len < capacity ? capacity - doc->text_pool_len : 0;
        if (len == 0) return;
    }
    if (memory__external_write(&doc->text_pool_object, doc->text_pool_len, text, len) != BRUCE_OK) {
        doc->truncated = true;
        return;
    }
    browser_item_t *item = browser_document__new_item(doc);
    if (item == NULL) return; /* Bytes are written but unreferenced; harmless. */
    item->kind = (uint8_t)BROWSER_ITEM_TEXT;
    item->text_offset = (uint16_t)doc->text_pool_len;
    item->text_len = (uint16_t)len;
    item->heading_level = (int8_t)heading_level;
    item->link_index = (int16_t)link_index;
    doc->text_pool_len += len;
}

int browser_document__add_link(browser_document_t *doc, const char *url) {
    if (doc == NULL || url == NULL) return -1;
    browser_link_t link;
    memset(&link, 0, sizeof(link));
    snprintf(link.url, sizeof(link.url), "%s", url);

    if (!browser_document__ext_reserve(
            &doc->links_object, (const void **)&doc->links, doc->link_count * sizeof(link), sizeof(link),
            BROWSER_INITIAL_LINK_CAP * sizeof(link), (size_t)BROWSER_MAX_LINKS * sizeof(link)
        )) {
        doc->truncated = true;
        return -1;
    }
    if (memory__external_write(&doc->links_object, doc->link_count * sizeof(link), &link, sizeof(link)) !=
        BRUCE_OK) {
        doc->truncated = true;
        return -1;
    }
    return (int)doc->link_count++;
}

void browser_document__add_image(browser_document_t *doc, const char *url, const char *alt, size_t alt_len) {
    if (doc == NULL || url == NULL) return;

    /* Built in a local first, then committed with one external_write() below
     * -- external storage can't be filled in place field-by-field the way
     * &doc->images[i] used to be, since a write there needs a fully-formed
     * value up front (see browser_document__ext_reserve()'s comment). */
    browser_image_ref_t image;
    memset(&image, 0, sizeof(image));
    snprintf(image.url, sizeof(image.url), "%s", url);
    if (alt != NULL && alt_len > 0) {
        if (alt_len >= sizeof(image.alt)) alt_len = sizeof(image.alt) - 1u;
        memcpy(image.alt, alt, alt_len);
        image.alt[alt_len] = '\0';
    }

    if (!browser_document__ext_reserve(
            &doc->images_object, (const void **)&doc->images, doc->image_count * sizeof(image), sizeof(image),
            BROWSER_INITIAL_IMAGE_CAP * sizeof(image), (size_t)BROWSER_MAX_IMAGES * sizeof(image)
        )) {
        doc->truncated = true;
        return;
    }
    if (memory__external_write(&doc->images_object, doc->image_count * sizeof(image), &image, sizeof(image)) !=
        BRUCE_OK) {
        doc->truncated = true;
        return;
    }

    browser_item_t *item = browser_document__new_item(doc);
    if (item == NULL) return; /* Bytes are written but unreferenced; harmless. */
    item->kind = (uint8_t)BROWSER_ITEM_IMAGE;
    item->image_index = (int16_t)doc->image_count++;
}

void browser_document__add_anchor(browser_document_t *doc, const char *name, size_t len) {
    if (doc == NULL || name == NULL || len == 0) return;
    browser_anchor_t anchor = {.item_index = (uint16_t)doc->item_count};
    if (len >= sizeof(anchor.name)) len = sizeof(anchor.name) - 1u;
    memcpy(anchor.name, name, len);
    anchor.name[len] = '\0';
    if (!browser_document__ext_reserve(
            &doc->anchors_object, (const void **)&doc->anchors, doc->anchor_count * sizeof(anchor), sizeof(anchor),
            BROWSER_INITIAL_ANCHOR_CAP * sizeof(anchor), (size_t)BROWSER_MAX_ANCHORS * sizeof(anchor)
        )) {
        doc->truncated = true;
        return;
    }
    if (memory__external_write(&doc->anchors_object, doc->anchor_count * sizeof(anchor), &anchor, sizeof(anchor)) ==
        BRUCE_OK) {
        doc->anchor_count++;
    }
}

bool browser_document__find_anchor(const browser_document_t *doc, const char *name, size_t *out_item_index) {
    if (doc == NULL || name == NULL || out_item_index == NULL) return false;
    for (size_t i = 0; i < doc->anchor_count; ++i) {
        if (strcmp(doc->anchors[i].name, name) == 0) {
            *out_item_index = doc->anchors[i].item_index;
            return true;
        }
    }
    return false;
}

void browser_document__add_landmark(browser_document_t *doc, browser_landmark_kind_t kind) {
    if (doc == NULL) return;
    int *slot = kind == BROWSER_LANDMARK_MAIN      ? &doc->main_item_index
                : kind == BROWSER_LANDMARK_ARTICLE ? &doc->article_item_index
                                                    : &doc->nav_item_index;
    if (*slot < 0) *slot = (int)doc->item_count;
}

void browser_document__add_break(browser_document_t *doc, bool paragraph) {
    if (doc == NULL) return;
    if (doc->item_count == 0) return; /* Nothing above to separate from -- would just be leading blank space. */

    /* Several block elements can close back to back (nested <div>s, a
     * heading forcing a break before it right after a paragraph's own
     * closing break, an image's own before/after breaks landing next to
     * markup that already emitted one, ...) -- browsers collapse that whole
     * run down to a single blank line, not one per closing tag, so do the
     * same: fold a new break into the last item instead of stacking another
     * one on, keeping paragraph (the stronger gap) if either call asked for
     * it. */
    browser_item_t *last = &doc->items[doc->item_count - 1];
    if (last->kind == BROWSER_ITEM_LINE_BREAK || last->kind == BROWSER_ITEM_PARAGRAPH_BREAK) {
        if (paragraph) last->kind = (uint8_t)BROWSER_ITEM_PARAGRAPH_BREAK;
        return;
    }

    browser_item_t *item = browser_document__new_item(doc);
    if (item == NULL) return;
    item->kind = (uint8_t)(paragraph ? BROWSER_ITEM_PARAGRAPH_BREAK : BROWSER_ITEM_LINE_BREAK);
}
