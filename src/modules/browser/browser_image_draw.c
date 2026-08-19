#include "browser_image_draw.h"

#include <stdint.h>

#include "core_sdk/image.h"
#include "core_sdk/memory.h"

bruce_result_t browser_image_draw__fit(
    const void *data, size_t len, int x, int y, int box_width, int box_height, bruce_display_color_t background,
    int *out_drawn_width, int *out_drawn_height
) {
    if (data == NULL || len == 0 || box_width <= 0 || box_height <= 0) return BRUCE_ERR_INVALID_ARGUMENT;

    /* fit=true bounds the *decoded* size to <= this process's display
     * viewport, which protects memory against a large source photo; the box
     * fit below is a second, smaller-still pass on top of that. */
    bruce_image_draw_options_t decode_options = {.center = false, .fit = true, .background = background};
    image_bitmap_t bitmap;
    bruce_result_t result = image__get_bitmap_from_memory(data, len, &decode_options, &bitmap);
    if (result != BRUCE_OK) return result;
    if (bitmap.pixels == NULL || bitmap.width == 0 || bitmap.height == 0) {
        image__bitmap_release(&bitmap);
        return BRUCE_ERR_UNSUPPORTED;
    }

    /* Scale down (never up) to fit the box, preserving aspect ratio. */
    uint32_t target_width = bitmap.width, target_height = bitmap.height;
    if (target_width > (uint32_t)box_width || target_height > (uint32_t)box_height) {
        if ((uint64_t)box_width * target_height <= (uint64_t)box_height * target_width) {
            target_height = (uint32_t)((uint64_t)target_height * (uint32_t)box_width / target_width);
            target_width = (uint32_t)box_width;
        } else {
            target_width = (uint32_t)((uint64_t)target_width * (uint32_t)box_height / target_height);
            target_height = (uint32_t)box_height;
        }
        if (target_width == 0) target_width = 1;
        if (target_height == 0) target_height = 1;
    }

    int draw_x = x + (box_width - (int)target_width) / 2;
    int draw_y = y + (box_height - (int)target_height) / 2;

    if (target_width == bitmap.width && target_height == bitmap.height) {
        result = display__draw_rgb_bitmap(
            (int16_t)draw_x, (int16_t)draw_y, bitmap.pixels, (int16_t)target_width, (int16_t)target_height
        );
    } else {
        size_t pixel_count = (size_t)target_width * (size_t)target_height;
        uint16_t *scaled = memory__malloc(pixel_count * sizeof(uint16_t));
        if (scaled == NULL) {
            image__bitmap_release(&bitmap);
            return BRUCE_ERR_NO_MEMORY;
        }
        /* Simple nearest-neighbor resample, the same approach
         * core/image/render.c's own (viewport-relative) scaling path uses. */
        for (uint32_t row = 0; row < target_height; ++row) {
            uint32_t source_row = (uint32_t)(((uint64_t)row * bitmap.height) / target_height);
            for (uint32_t col = 0; col < target_width; ++col) {
                uint32_t source_col = (uint32_t)(((uint64_t)col * bitmap.width) / target_width);
                scaled[(size_t)row * target_width + col] =
                    bitmap.pixels[(size_t)source_row * bitmap.width + source_col];
            }
        }
        result = display__draw_rgb_bitmap(
            (int16_t)draw_x, (int16_t)draw_y, scaled, (int16_t)target_width, (int16_t)target_height
        );
        memory__free(scaled);
    }

    image__bitmap_release(&bitmap);
    if (result == BRUCE_OK) {
        if (out_drawn_width != NULL) *out_drawn_width = (int)target_width;
        if (out_drawn_height != NULL) *out_drawn_height = (int)target_height;
    }
    return result;
}
