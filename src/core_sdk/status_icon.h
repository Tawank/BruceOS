#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

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

/* The registry is global and unrestricted. Existing keys are replaced. */
bruce_result_t status_icon__push(const char *key, const uint8_t *bitmap,
                                  uint8_t width, uint8_t height);
bruce_result_t status_icon__remove(const char *key);

/* Returns the total entry count and current revision. Up to capacity entries
 * are copied in lexicographic key order. */
bruce_result_t status_icon__list(bruce_status_icon_t *icons, size_t capacity,
                                  size_t *out_count, uint32_t *out_revision);
