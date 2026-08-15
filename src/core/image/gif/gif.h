#pragma once

#include "core/image/image.h"

bruce_result_t image__decode_gif(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
);
