#pragma once

#include "core/image/image.h"
#include "core_sdk/storage.h"

bruce_result_t image__decode_png(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
);
bruce_result_t image__decode_png_file(
    bruce_file_id_t file, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
);
