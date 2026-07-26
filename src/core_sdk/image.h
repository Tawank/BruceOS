#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/display.h"
#include "core_sdk/result.h"

typedef enum {
    BRUCE_IMAGE_FORMAT_JPEG,
    BRUCE_IMAGE_FORMAT_PNG,
    BRUCE_IMAGE_FORMAT_GIF,
} bruce_image_format_t;

typedef struct {
    bruce_image_format_t format;
    uint16_t width;
    uint16_t height;
} bruce_image_info_t;

typedef struct {
    int16_t x;
    int16_t y;
    bool center;
    bool fit;
    bruce_display_color_t background;
} bruce_image_draw_options_t;

/* Draws JPEG, PNG, or the first frame of a GIF into the caller's viewport.
 * `fit` preserves aspect ratio and only scales down. Transparent pixels are
 * composited over `background`. The caller controls frame presentation. */
bruce_result_t image__draw_memory(const void *data, size_t size,
                                  const bruce_image_draw_options_t *options,
                                  bruce_image_info_t *out_info);

/* Reads and draws an image through task-owned Core storage. */
bruce_result_t image__draw_path(const char *path,
                                const bruce_image_draw_options_t *options,
                                bruce_image_info_t *out_info);

/* Returns true for the filename extensions accepted by image__draw_path(). */
bool image__is_supported_path(const char *path);
