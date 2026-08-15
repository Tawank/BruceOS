#include "core/image/image.h"

#include <limits.h>

#include "core_sdk/memory.h"

void image__bitmap_release(image_bitmap_t *bitmap) {
    if (bitmap == NULL) return;
    if (bitmap->backing.backend != BRUCE_MEMORY_BACKEND_INVALID)
        (void)memory__external_free(&bitmap->backing);
    else memory__free(bitmap->pixels);
    *bitmap = (image_bitmap_t){0};
}

void image__fit_size(
    uint32_t source_width, uint32_t source_height, bool fit, uint16_t *out_width, uint16_t *out_height
) {
    uint32_t width = source_width, height = source_height;
    uint32_t viewport_width = (uint32_t)display__width(), viewport_height = (uint32_t)display__height();
    if (fit && viewport_width > 0 && viewport_height > 0) {
        if ((uint64_t)viewport_width * height <= (uint64_t)viewport_height * width) {
            height = (height * viewport_width) / width;
            width = viewport_width;
        } else {
            width = (width * viewport_height) / height;
            height = viewport_height;
        }
    }
    *out_width = (uint16_t)(width > UINT16_MAX ? UINT16_MAX : width);
    *out_height = (uint16_t)(height > UINT16_MAX ? UINT16_MAX : height);
}

bruce_result_t image__draw_pixels(
    const uint16_t *pixels, uint16_t width, uint16_t height, const bruce_image_draw_options_t *options
) {
    if (width > INT16_MAX || height > INT16_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    int x = options->x, y = options->y;
    if (options->center) {
        x += (display__width() - width) / 2;
        y += (display__height() - height) / 2;
    }
    return display__draw_rgb_bitmap((int16_t)x, (int16_t)y, pixels, (int16_t)width, (int16_t)height);
}

bruce_result_t image__draw_scaled_pixels(
    const uint16_t *pixels, uint16_t width, uint16_t height, const bruce_image_draw_options_t *options
) {
    uint16_t out_width, out_height;
    image__fit_size(width, height, options->fit, &out_width, &out_height);
    if (out_width == width && out_height == height) return image__draw_pixels(pixels, width, height, options);
    if (out_width == 0 || out_height == 0 ||
        (size_t)out_width > SIZE_MAX / ((size_t)out_height * sizeof(uint16_t)))
        return BRUCE_ERR_RESOURCE_LIMIT;
    uint16_t *scaled = memory__malloc((size_t)out_width * out_height * sizeof(*scaled));
    if (scaled == NULL) return BRUCE_ERR_NO_MEMORY;
    for (uint16_t y = 0; y < out_height; ++y) {
        uint32_t source_y = (uint32_t)(((uint64_t)y * height) / out_height);
        for (uint16_t x = 0; x < out_width; ++x) {
            uint32_t source_x = (uint32_t)(((uint64_t)x * width) / out_width);
            scaled[(size_t)y * out_width + x] = pixels[(size_t)source_y * width + source_x];
        }
    }
    bruce_result_t result = image__draw_pixels(scaled, out_width, out_height, options);
    memory__free(scaled);
    return result;
}
