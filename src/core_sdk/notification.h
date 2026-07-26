#pragma once

#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_NOTIFICATION_TEXT_MAX 96
#define BRUCE_NOTIFICATION_DURATION_MIN_MS 250u
#define BRUCE_NOTIFICATION_DURATION_MAX_MS 30000u

/* Shows or replaces the global transient notification. Text is copied before
 * return. Duration is clamped to the public minimum and maximum. */
bruce_result_t notification__push(const char *text, uint32_t duration_ms);

/* Dismisses the current notification. This operation is idempotent. */
bruce_result_t notification__dismiss(void);
