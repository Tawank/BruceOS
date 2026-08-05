#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_NOTIFICATION_TEXT_MAX 96
#define BRUCE_NOTIFICATION_DURATION_MIN_MS 250u
#define BRUCE_NOTIFICATION_DURATION_MAX_MS 30000u

/* Shows or replaces the global transient notification. Text is copied before
 * return. Duration is clamped to the public minimum and maximum. This is a
 * fire-and-forget request: it is queued for whatever is currently consuming
 * notification__wait_request() below (normally modules/notification) and
 * always returns BRUCE_OK, whether or not anything is listening yet. Only
 * the single latest unread push/dismiss is kept -- an older one still
 * unread is replaced, not queued behind it. */
bruce_result_t notification__push(const char *text, uint32_t duration_ms);

/* Dismisses the current notification. This operation is idempotent. */
bruce_result_t notification__dismiss(void);

/* Consumer side of the mailbox notification__push()/dismiss() write into.
 * Blocks up to `timeout_ms` (UINT32_MAX blocks forever) for the next
 * request. On a push, copies its text into `out_text` (size `text_size`,
 * always NUL-terminated) and its duration into `*out_duration_ms`, and sets
 * `*out_dismiss` to false; on a dismiss, sets `*out_dismiss` to true and
 * leaves `out_text`/`*out_duration_ms` untouched. Returns BRUCE_ERR_TIMEOUT
 * if nothing arrived in time.
 *
 * Built-in callers only (BRUCE_ERR_PERMISSION otherwise): this is the
 * hand-off point for whatever process renders notifications -- normally
 * modules/notification, which uses only display__overlay_* to do it, so
 * nothing about rendering is special-cased in Core. A user who wants a
 * different notification UI does not need this function at all: they can
 * just call display__overlay_create() directly from their own app instead
 * of going through notification__push(). */
bruce_result_t notification__wait_request(
    char *out_text, size_t text_size, uint32_t *out_duration_ms, bool *out_dismiss, uint32_t timeout_ms
);
