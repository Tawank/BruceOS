#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/process.h"
#include "core_sdk/result.h"

#define BRUCE_PUBSUB_TOPIC_MAX 32
#define BRUCE_PUBSUB_MESSAGE_MAX 32

typedef struct {
    uint8_t data[BRUCE_PUBSUB_MESSAGE_MAX];
    size_t size;
    uint64_t timestamp_ms;
} bruce_pubsub_message_t;

/*
 * A minimal named-topic broadcast bus for Core-owned hardware services (e.g.
 * a shared I2C bus driver) to publish semantic events - touch taps, sensor
 * readings - that any number of other processes, including ELF/JS apps, can
 * subscribe to without ever touching the underlying hardware or bus
 * themselves. It exists so that when a physical resource can only safely
 * have one owner (see core/device/board_i2c.h), that owner is the only
 * thing that ever opens it; everyone else reads its output as messages.
 *
 * Topics are plain strings with no reservation system. Publishing is
 * restricted to built-in Core processes so an ELF/JS app cannot forge, e.g.,
 * "device.touch" and fool another app; any process may subscribe.
 */

/* Publishes `data` (up to BRUCE_PUBSUB_MESSAGE_MAX bytes) to `topic`,
 * delivered to every subscription currently open on that topic. Requires the
 * calling process to be a Core built-in. If a subscriber's queue is full,
 * its oldest unread message is dropped to make room for this one -
 * subscribers are expected to drain promptly. Returns BRUCE_ERR_PERMISSION
 * for a non-built-in caller, BRUCE_ERR_INVALID_ARGUMENT for a NULL/empty
 * topic or an oversized message. */
bruce_result_t pubsub__publish(const char *topic, const void *data, size_t size);

/* Subscribes the calling process to `topic`. Delivery starts from the first
 * message published after this call returns; nothing published earlier is
 * replayed. */
bruce_result_t pubsub__subscribe(const char *topic, bruce_pubsub_id_t *out_sub);

/* Ends a subscription. Subscriptions also close automatically when the
 * owning process exits. Returns BRUCE_ERR_NOT_FOUND if `sub` does not belong
 * to the calling process. */
bruce_result_t pubsub__unsubscribe(bruce_pubsub_id_t sub);

/* Pops the next queued message for `sub`.
 *
 * `timeout_ms` controls blocking:
 *   - 0      : non-blocking; returns BRUCE_ERR_TIMEOUT immediately if none queued.
 *   - >0     : blocks up to the requested total elapsed milliseconds.
 *   - 0xFFFFFFFF (portMAX_DELAY equivalent) : blocks until a message arrives.
 *
 * Returns BRUCE_ERR_NOT_FOUND if `sub` does not belong to the caller.
 */
bruce_result_t pubsub__read(bruce_pubsub_id_t sub, bruce_pubsub_message_t *out_message, uint32_t timeout_ms);
