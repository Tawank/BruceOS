#include "browser_history.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"

/* Read-only access to entry `index`; only valid once entries is
 * allocated (i.e. after at least one push()). */
static const char *browser_history__entry(const browser_history_t *history, int index) {
    return history->entries + (size_t)index * BROWSER_URL_MAX;
}

bruce_result_t browser_history__create(browser_history_t **out_history) {
    if (out_history == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    browser_history_t *history = memory__malloc(sizeof(*history));
    if (history == NULL) return BRUCE_ERR_NO_MEMORY;
    memset(history, 0, sizeof(*history));
    history->count = 0;
    history->current = -1;
    /* Allocated fully up front, not lazily grown -- it's small and fixed
     * size, so there's no doubling/migration to do (see
     * browser_document.h's ext_reserve()-based fields for the growable
     * case). */
    history->entries = memory__external_malloc((size_t)BROWSER_HISTORY_MAX * BROWSER_URL_MAX);
    if (history->entries == NULL) {
        memory__free(history);
        return BRUCE_ERR_NO_MEMORY;
    }
    *out_history = history;
    return BRUCE_OK;
}

void browser_history__destroy(browser_history_t *history) {
    if (history == NULL) return;
    if (history->entries != NULL) (void)memory__external_free(history->entries);
    memory__free(history);
}

void browser_history__push(browser_history_t *history, const char *url) {
    if (history == NULL || url == NULL) return;

    /* Navigating from the middle of history discards the forward branch,
     * same as any other browser. */
    history->count = history->current + 1;

    if (history->count >= BROWSER_HISTORY_MAX) {
        /* Drop the oldest entry, shifting the rest down by one. Each entry
         * is copied out to a small on-stack buffer first rather than moved
         * directly within entries: memory__external_memcpy() rejects a
         * source that aliases the destination allocation's own range (see
         * core/memory/memory_external.c), which a direct
         * entries[i] -> entries[i - 1] copy would be. */
        char slot[BROWSER_URL_MAX];
        for (int i = 1; i < BROWSER_HISTORY_MAX; ++i) {
            memcpy(slot, browser_history__entry(history, i), sizeof(slot));
            (void)memory__external_memcpy(
                history->entries, (size_t)(i - 1) * BROWSER_URL_MAX, slot, sizeof(slot)
            );
        }
        history->count = BROWSER_HISTORY_MAX - 1;
    }

    char entry[BROWSER_URL_MAX];
    snprintf(entry, sizeof(entry), "%s", url);
    (void)memory__external_memcpy(
        history->entries, (size_t)history->count * BROWSER_URL_MAX, entry, sizeof(entry)
    );
    history->current = history->count;
    history->count++;
}

bool browser_history__can_go_back(const browser_history_t *history) { return history != NULL && history->current > 0; }

bool browser_history__can_go_forward(const browser_history_t *history) {
    return history != NULL && history->current >= 0 && history->current + 1 < history->count;
}

const char *browser_history__back(browser_history_t *history) {
    if (!browser_history__can_go_back(history)) return NULL;
    history->current--;
    return browser_history__entry(history, history->current);
}

const char *browser_history__forward(browser_history_t *history) {
    if (!browser_history__can_go_forward(history)) return NULL;
    history->current++;
    return browser_history__entry(history, history->current);
}

const char *browser_history__current(const browser_history_t *history) {
    if (history == NULL || history->current < 0) return NULL;
    return browser_history__entry(history, history->current);
}
