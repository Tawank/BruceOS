#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/display.h"
#include "core_sdk/memory.h"
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

typedef struct {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint16_t source_width;
    uint16_t source_height;
    bruce_image_format_t format;
    bruce_memory_object_t backing;
} image_bitmap_t;

typedef struct bruce_gif bruce_gif_t;

/* Decodes JPEG, PNG, or the first frame of a GIF into an owned RGB565 bitmap.
 * `fit` preserves aspect ratio and only scales down. Transparent pixels are
 * composited over `background`. Release a successful result with
 * image__bitmap_release(). */
bruce_result_t image__get_bitmap_from_memory(
    const void *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *out_bitmap
);

/* Resizes `source` into an owned RGB565 bitmap, preserving aspect ratio
 * while fitting within `max_width` x `max_height`. Core chooses the backing
 * store; callers must not depend on a particular memory backend. The result is
 * never upscaled and must be
 * released with image__bitmap_release(). */
bruce_result_t image__bitmap_resize(
    const image_bitmap_t *source, uint16_t max_width, uint16_t max_height, image_bitmap_t *out_bitmap
);

/* Opens and decodes an image through process-owned Core storage. */
bruce_result_t image__get_bitmap_from_file(
    const char *path, const bruce_image_draw_options_t *options, image_bitmap_t *out_bitmap
);

/* Decodes, draws, and releases an image from Core storage. */
bruce_result_t
image__draw_path(const char *path, const bruce_image_draw_options_t *options, bruce_image_info_t *out_info);

/* Draws an owned bitmap into the active render target without releasing it.
 * The caller controls frame presentation. */
bruce_result_t image__draw_bitmap(const image_bitmap_t *bitmap, const bruce_image_draw_options_t *options);

void image__bitmap_release(image_bitmap_t *bitmap);

/* Returns true for the filename extensions accepted by image__get_bitmap_from_file(). */
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
