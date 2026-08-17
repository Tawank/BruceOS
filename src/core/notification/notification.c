#include "core_sdk/notification.h"

#include <string.h>

#include "core/process/process.h"
#include "core_sdk/stdio.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/queue.h"

typedef struct {
    bool dismiss; /* false: text/duration_ms/gui_requested/session below are the push payload. */
    char text[BRUCE_NOTIFICATION_TEXT_MAX];
    uint32_t duration_ms;
    /* Whether the *pushing* process was launched with GUI=1, captured once
     * here at push time (see process_registry__current_context()) since
     * that's the only place the queue crosses from the pusher's task into
     * the consumer's -- the consumer has no way to ask afterwards which
     * process a given queued message came from. Lets the consumer (see
     * notification__wait_request()) decide whether to render a GUI banner
     * or print to console without needing to know anything about wifi,
     * bluetooth, or any other specific pusher. */
    bool gui_requested;
    /* The pushing process's own routed stdio session (see
     * process_registry__current_stdio_session()), captured for the same
     * reason as gui_requested above: the consumer runs as its own
     * background process, so its *own* routed session (if any) has nothing
     * to do with the pusher's. Without this, a console-fallback print would
     * land on whatever the consumer happens to be routed to -- normally
     * nothing, i.e. the physical serial console -- instead of the terminal
     * (or other session) the pusher was actually running in. */
    bruce_stdio_session_t session;
} notification__message_t;

/* Depth-1 "mailbox" queue: xQueueOverwrite() always replaces whatever
 * request hasn't been consumed yet, so notification__wait_request() only
 * ever sees the latest push or dismiss, matching notification__push()'s
 * documented last-writer-wins behavior. Created lazily on first use, like
 * the lock-creation pattern used throughout core/display -- see
 * display__ensure_lock() for the same convention. */
static QueueHandle_t s_queue;

static QueueHandle_t notification__ensure_queue(void) {
    if (s_queue == NULL) s_queue = xQueueCreate(1, sizeof(notification__message_t));
    return s_queue;
}

bruce_result_t notification__push(const char *text, uint32_t duration_ms) {
    if (text == NULL || text[0] == '\0' || strlen(text) >= BRUCE_NOTIFICATION_TEXT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (duration_ms < BRUCE_NOTIFICATION_DURATION_MIN_MS) duration_ms = BRUCE_NOTIFICATION_DURATION_MIN_MS;
    if (duration_ms > BRUCE_NOTIFICATION_DURATION_MAX_MS) duration_ms = BRUCE_NOTIFICATION_DURATION_MAX_MS;
    QueueHandle_t queue = notification__ensure_queue();
    if (queue == NULL) return BRUCE_ERR_NO_MEMORY;
    bool gui_requested = false;
    (void)process_registry__current_context(NULL, NULL, 0, &gui_requested);
    notification__message_t message = {0};
    message.dismiss = false;
    strncpy(message.text, text, sizeof(message.text) - 1);
    message.duration_ms = duration_ms;
    message.gui_requested = gui_requested;
    message.session = process_registry__current_stdio_session();
    (void)xQueueOverwrite(queue, &message);
    return BRUCE_OK;
}

bruce_result_t notification__dismiss(void) {
    QueueHandle_t queue = notification__ensure_queue();
    if (queue == NULL) return BRUCE_ERR_NO_MEMORY;
    notification__message_t message = {0};
    message.dismiss = true;
    (void)xQueueOverwrite(queue, &message);
    return BRUCE_OK;
}

bruce_result_t notification__wait_request(
    char *out_text, size_t text_size, uint32_t *out_duration_ms, bool *out_dismiss, bool *out_gui_requested,
    bruce_stdio_session_t *out_session, uint32_t timeout_ms
) {
    if (out_dismiss == NULL || (out_text != NULL) != (text_size > 0) ||
        (out_text != NULL) != (out_duration_ms != NULL) || (out_text != NULL) != (out_gui_requested != NULL) ||
        (out_text != NULL) != (out_session != NULL)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bool built_in = false;
    if (process_registry__current_context(&built_in, NULL, 0, NULL) != BRUCE_OK || !built_in) {
        return BRUCE_ERR_PERMISSION;
    }
    QueueHandle_t queue = notification__ensure_queue();
    if (queue == NULL) return BRUCE_ERR_NO_MEMORY;
    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    notification__message_t message;
    if (xQueueReceive(queue, &message, ticks) != pdTRUE) return BRUCE_ERR_TIMEOUT;
    *out_dismiss = message.dismiss;
    if (!message.dismiss && out_text != NULL) {
        strncpy(out_text, message.text, text_size - 1);
        out_text[text_size - 1] = '\0';
        *out_duration_ms = message.duration_ms;
        *out_gui_requested = message.gui_requested;
        *out_session = message.session;
    }
    return BRUCE_OK;
}
