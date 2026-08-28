#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

/**
 * @brief Global transient notification mailbox (push/dismiss and its consumer).
 */

#define BRUCE_NOTIFICATION_TEXT_MAX 96
#define BRUCE_NOTIFICATION_DURATION_MIN_MS 250u
#define BRUCE_NOTIFICATION_DURATION_MAX_MS 30000u

/**
 * @brief Shows or replaces the global transient notification.
 *
 * Text is copied before return. Duration is clamped to the public minimum
 * and maximum. This is a fire-and-forget request: it is queued for
 * whatever is currently consuming notification__wait_request() below
 * (normally modules/notification_service) and always returns BRUCE_OK,
 * whether or not anything is listening yet. Only the single latest unread
 * push/dismiss is kept -- an older one still unread is replaced, not
 * queued behind it.
 *
 * Callers don't choose or need to know how the notification will be shown:
 * whether the calling process was launched with GUI interaction (see
 * core_sdk/runtime.h's runtime__gui_requested()) is captured here, at push
 * time, and travels with the request so the consumer can render a GUI
 * banner or print a console line as appropriate -- see
 * notification__wait_request().
 *
 * @param text Notification text (copied before return).
 * @param duration_ms How long to show it, clamped to BRUCE_NOTIFICATION_DURATION_MIN/MAX_MS.
 */
bruce_result_t notification__push(const char *text, uint32_t duration_ms);

/** @brief Dismisses the current notification. This operation is idempotent. */
bruce_result_t notification__dismiss(void);

/**
 * @brief Consumer side of the mailbox notification__push()/dismiss() write into.
 *
 * Blocks up to `timeout_ms` (UINT32_MAX blocks forever) for the next
 * request. On a push, copies its text into `out_text` (size `text_size`,
 * always NUL-terminated), its duration into `*out_duration_ms`, whether the
 * pushing process wanted GUI interaction into `*out_gui_requested`, and the
 * pushing process's own routed stdio session (BRUCE_STDIO_SESSION_INVALID
 * if it had none) into `*out_session`, and sets `*out_dismiss` to false; on
 * a dismiss, sets `*out_dismiss` to true and leaves the other four
 * untouched. Returns BRUCE_ERR_TIMEOUT if nothing arrived in time.
 *
 * This is the hand-off point for whatever process renders notifications --
 * normally modules/notification_service, which uses `*out_gui_requested`
 * to choose between a GUI banner (display__overlay_*) and a console line
 * (stdio__write_to(*out_session, ...) -- not stdio__write()/stdio__printf(),
 * since the consumer's own routed session, if any, is unrelated to the
 * pusher's), so nothing about rendering is special-cased in Core. A user
 * who wants a different notification UI does not need this function at
 * all: they can just call display__overlay_create() directly from their
 * own app instead of going through notification__push().
 *
 * @param out_text Receives the pushed notification text (NUL-terminated).
 * @param text_size Size of out_text in bytes.
 * @param out_duration_ms Receives the pushed notification's requested duration.
 * @param out_dismiss Set to true if this was a dismiss request rather than a push.
 * @param out_gui_requested Receives whether the pushing process wanted GUI interaction.
 * @param out_session Receives the pushing process's routed stdio session, or BRUCE_STDIO_SESSION_INVALID.
 * @param timeout_ms Time to wait for the next request, in milliseconds (UINT32_MAX blocks forever).
 * @permission built-in only (external callers get BRUCE_ERR_PERMISSION)
 */
bruce_result_t notification__wait_request(
    char *out_text, size_t text_size, uint32_t *out_duration_ms, bool *out_dismiss, bool *out_gui_requested,
    bruce_stdio_session_t *out_session, uint32_t timeout_ms
);
