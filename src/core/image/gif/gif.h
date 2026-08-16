#pragma once

#include "core/image/image.h"

struct bruce_gif {
    const uint8_t *data;
    size_t size;
    image_reader_t reader;
    size_t frames_offset;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t canvas_width;
    uint16_t canvas_height;
    uint16_t global_palette[256];
    size_t global_palette_count;
    uint16_t *canvas;
    uint16_t *restore_canvas;
    bruce_image_draw_options_t options;
    bruce_memory_object_t backing;
    uint32_t delay_ms;
    uint16_t previous_left;
    uint16_t previous_top;
    uint16_t previous_width;
    uint16_t previous_height;
    uint8_t previous_disposal;
};

bruce_result_t gif__open_memory(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options,
    const bruce_memory_object_t *backing, bruce_gif_t **out_gif
);

bruce_result_t image__decode_gif(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
);
