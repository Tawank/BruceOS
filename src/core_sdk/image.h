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

typedef struct bruce_gif bruce_gif_t;

/* Draws JPEG, PNG, or the first frame of a GIF into the caller's viewport.
 * `fit` preserves aspect ratio and only scales down. Transparent pixels are
 * composited over `background`. The caller controls frame presentation. */
bruce_result_t image__draw_memory(
    const void *data, size_t size, const bruce_image_draw_options_t *options, bruce_image_info_t *out_info
);

/* Reads and draws an image through process-owned Core storage. */
bruce_result_t
image__draw_path(const char *path, const bruce_image_draw_options_t *options, bruce_image_info_t *out_info);

/* Returns true for the filename extensions accepted by image__draw_path(). */
bool image__is_supported_path(const char *path);

/* Opens an animated GIF from Core storage and decodes its first frame. The
 * returned object owns its file data and must be released with
 * image__gif_close(). */
bruce_result_t image__gif_open(
    const char *path, const bruce_image_draw_options_t *options, bruce_gif_t **out_gif,
    bruce_image_info_t *out_info
);

/* Draws the current composited frame and optionally returns its display time. */
bruce_result_t image__gif_draw(bruce_gif_t *gif, uint32_t *out_delay_ms);

/* Advances and decodes the next frame. At the trailer this loops to the first
 * frame and sets `out_looped`, when non-NULL, to true. */
bruce_result_t image__gif_increment(bruce_gif_t *gif, bool *out_looped);

void image__gif_close(bruce_gif_t *gif);
