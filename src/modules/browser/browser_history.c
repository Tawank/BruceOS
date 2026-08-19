#include "browser_history.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"

bruce_result_t browser_history__create(browser_history_t **out_history) {
    if (out_history == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    browser_history_t *history = memory__malloc(sizeof(*history));
    if (history == NULL) return BRUCE_ERR_NO_MEMORY;
    history->count = 0;
    history->current = -1;
    *out_history = history;
    return BRUCE_OK;
}

void browser_history__destroy(browser_history_t *history) { memory__free(history); }

void browser_history__push(browser_history_t *history, const char *url) {
    if (history == NULL || url == NULL) return;

    /* Navigating from the middle of history discards the forward branch,
     * same as any other browser. */
    history->count = history->current + 1;

    if (history->count >= BROWSER_HISTORY_MAX) {
        memmove(history->entries[0], history->entries[1], (size_t)(BROWSER_HISTORY_MAX - 1) * BROWSER_URL_MAX);
        history->count = BROWSER_HISTORY_MAX - 1;
    }
    snprintf(history->entries[history->count], BROWSER_URL_MAX, "%s", url);
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
    return history->entries[history->current];
}

const char *browser_history__forward(browser_history_t *history) {
    if (!browser_history__can_go_forward(history)) return NULL;
    history->current++;
    return history->entries[history->current];
}

const char *browser_history__current(const browser_history_t *history) {
    if (history == NULL || history->current < 0) return NULL;
    return history->entries[history->current];
}
