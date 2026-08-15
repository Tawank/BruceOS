#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/image.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} image_reader_t;

typedef struct {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint16_t source_width;
    uint16_t source_height;
    bruce_image_format_t format;
} image_bitmap_t;

bool image__reader_read(image_reader_t *reader, void *out, size_t size);
bool image__reader_read_u8(image_reader_t *reader, uint8_t *out);
bool image__reader_read_u16(image_reader_t *reader, uint16_t *out);
bool image__reader_skip(image_reader_t *reader, size_t size);

void image__fit_size(
    uint32_t source_width, uint32_t source_height, bool fit, uint16_t *out_width, uint16_t *out_height
);
bruce_result_t image__draw_pixels(
    const uint16_t *pixels, uint16_t width, uint16_t height, const bruce_image_draw_options_t *options
);
bruce_result_t image__draw_scaled_pixels(
    const uint16_t *pixels, uint16_t width, uint16_t height, const bruce_image_draw_options_t *options
);
void image__bitmap_release(image_bitmap_t *bitmap);
