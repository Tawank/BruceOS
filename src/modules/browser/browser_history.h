#pragma once

/* Back/forward navigation history: a plain array of visited URLs with a
 * "current" cursor, the same model every desktop/mobile browser uses.
 * Navigating to a new URL while the cursor isn't at the end discards the
 * discarded-forward entries, matching that same familiar behavior.
 *
 * BROWSER_HISTORY_MAX * BROWSER_URL_MAX (24 * 400 = 9.6 KiB) would otherwise
 * be a single fixed internal-heap block held for the app's entire lifetime,
 * regardless of how many pages were actually visited -- see
 * browser_document.h's comment for why that's worth avoiding on a board with
 * no PSRAM. It's rarely touched (only on an actual navigation, never on a
 * redraw), so unlike browser_document.h's items array there's no read-hot-path
 * reason to keep it internal. */

#include <stdbool.h>

#include "browser_document.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"

#define BROWSER_HISTORY_MAX 24

typedef struct {
    bruce_memory_object_t entries_object; /* BROWSER_HISTORY_MAX * BROWSER_URL_MAX bytes, external. */
    const char *entries;                  /* Mapped read pointer; entries[i * BROWSER_URL_MAX]. */
    int count;
    int current; /* -1 when empty. */
} browser_history_t;

bruce_result_t browser_history__create(browser_history_t **out_history);
void browser_history__destroy(browser_history_t *history);

/* Records a navigation to `url`, becoming the new current entry. Drops the
 * oldest entry once BROWSER_HISTORY_MAX is reached. */
void browser_history__push(browser_history_t *history, const char *url);

bool browser_history__can_go_back(const browser_history_t *history);
bool browser_history__can_go_forward(const browser_history_t *history);

/* Move the cursor and return the URL now current, or NULL if that direction
 * isn't available. */
const char *browser_history__back(browser_history_t *history);
const char *browser_history__forward(browser_history_t *history);

/* The current entry's URL, or NULL if the history is empty. */
const char *browser_history__current(const browser_history_t *history);
