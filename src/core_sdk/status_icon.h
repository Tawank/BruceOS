#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Status-bar icon registry: small 1bpp bitmaps published by key.
 *
 * Not permission-gated; the registry is global and unrestricted.
 */

#define BRUCE_STATUS_ICON_KEY_MAX 32
#define BRUCE_STATUS_ICON_MAX 16
#define BRUCE_STATUS_ICON_MAX_WIDTH 16
#define BRUCE_STATUS_ICON_MAX_HEIGHT 16
#define BRUCE_STATUS_ICON_BITMAP_MAX 32

/* Bitmaps are 1bpp, row-major, MSB-first, with each row byte-aligned. */
typedef struct {
    char key[BRUCE_STATUS_ICON_KEY_MAX];
    uint8_t width;
    uint8_t height;
    uint8_t bitmap[BRUCE_STATUS_ICON_BITMAP_MAX];
} bruce_status_icon_t;

/**
 * @brief Publishes/replaces the icon at `key`.
 *
 * The registry is global and unrestricted. Existing keys are replaced.
 *
 * @param key Icon key to publish under.
 * @param bitmap 1bpp, row-major, MSB-first bitmap data, each row byte-aligned.
 * @param width Bitmap width in pixels (up to BRUCE_STATUS_ICON_MAX_WIDTH).
 * @param height Bitmap height in pixels (up to BRUCE_STATUS_ICON_MAX_HEIGHT).
 */
bruce_result_t status_icon__push(const char *key, const uint8_t *bitmap, uint8_t width, uint8_t height);

/**
 * @brief Looks up a built-in icon by name, scales it to fit, and publishes it under key.
 *
 * Scales it proportionally to fit the status bar limit.
 *
 * @param key Icon key to publish under.
 * @param icon_name Built-in icon name (see icon.h).
 */
bruce_result_t status_icon__push_named(const char *key, const char *icon_name);

/**
 * @brief Removes the icon published at `key`.
 *
 * @param key Icon key to remove.
 */
bruce_result_t status_icon__remove(const char *key);

/**
 * @brief Returns the total entry count and current revision.
 *
 * Up to capacity entries are copied in lexicographic key order.
 *
 * @param icons Array to receive icon entries.
 * @param capacity Number of entries the icons array can hold.
 * @param out_count Receives the total number of published icons.
 * @param out_revision Receives the current registry revision.
 */
bruce_result_t
status_icon__list(bruce_status_icon_t *icons, size_t capacity, size_t *out_count, uint32_t *out_revision);

/**
 * @brief Copies one entry by its lexicographic list index.
 *
 * Without requiring callers to reserve a full BRUCE_STATUS_ICON_MAX
 * snapshot. The optional revision is captured under the same lock as the
 * icon.
 *
 * @param index Zero-based lexicographic index of the icon to copy.
 * @param out_icon Receives the icon entry.
 * @param out_revision Receives the current registry revision, or NULL.
 */
bruce_result_t
status_icon__get(size_t index, bruce_status_icon_t *out_icon, uint32_t *out_revision);
