#include "gif.h"

#include <string.h>

#include "core/image/image.h"
#include "core_sdk/memory.h"

#define GIF_DICTIONARY_SIZE 4096

static bool gif__skip_blocks(image_reader_t *reader) {
    uint8_t count = 1;
    do {
        if (!image__reader_read_u8(reader, &count) || !image__reader_skip(reader, count)) return false;
    } while (count != 0);
    return true;
}

static bool gif__palette(image_reader_t *reader, uint16_t *palette, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint8_t rgb[3];
        if (!image__reader_read(reader, rgb, sizeof(rgb))) return false;
        palette[i] = display__color565(rgb[0], rgb[1], rgb[2]);
    }
    return true;
}

typedef struct {
    uint16_t *pixels, canvas_width, canvas_height, source_width, source_height;
    uint16_t left, top, frame_width, frame_height;
    uint16_t palette[256];
    size_t palette_count;
    int transparent_index;
    bool interlaced, invalid_output;
    size_t pixel_index;
} gif_output_t;

static uint16_t gif__interlaced_row(uint16_t row, uint16_t height) {
    static const uint8_t starts[] = {0, 4, 2, 1};
    static const uint8_t steps[] = {8, 8, 4, 2};
    uint16_t index = 0;
    for (size_t pass = 0; pass < 4; ++pass)
        for (uint32_t y = starts[pass]; y < height; y += steps[pass])
            if (index++ == row) return y;
    return row;
}

static void gif__emit(gif_output_t *output, uint8_t color) {
    if (output->pixel_index >= (size_t)output->frame_width * output->frame_height ||
        color >= output->palette_count) {
        output->invalid_output = true;
        return;
    }
    uint16_t x = (uint16_t)(output->pixel_index % output->frame_width);
    uint16_t y = (uint16_t)(output->pixel_index / output->frame_width);
    output->pixel_index++;
    if (output->interlaced) y = gif__interlaced_row(y, output->frame_height);
    if (color == output->transparent_index) return;
    uint32_t source_x = output->left + x, source_y = output->top + y;
    if (source_x >= output->source_width || source_y >= output->source_height) return;
    /* Fill the whole destination box this source pixel scales to, not just a single point, so
     * upscaling doesn't leave unwritten (background-colored) gaps between mapped pixels. */
    uint32_t out_x0 = (uint32_t)(((uint64_t)source_x * output->canvas_width) / output->source_width);
    uint32_t out_x1 = (uint32_t)((((uint64_t)source_x + 1) * output->canvas_width) / output->source_width);
    uint32_t out_y0 = (uint32_t)(((uint64_t)source_y * output->canvas_height) / output->source_height);
    uint32_t out_y1 = (uint32_t)((((uint64_t)source_y + 1) * output->canvas_height) / output->source_height);
    if (out_x1 <= out_x0) out_x1 = out_x0 + 1;
    if (out_y1 <= out_y0) out_y1 = out_y0 + 1;
    if (out_x1 > output->canvas_width) out_x1 = output->canvas_width;
    if (out_y1 > output->canvas_height) out_y1 = output->canvas_height;
    for (uint32_t out_y = out_y0; out_y < out_y1; ++out_y)
        for (uint32_t out_x = out_x0; out_x < out_x1; ++out_x)
            output->pixels[(size_t)out_y * output->canvas_width + out_x] = output->palette[color];
}

static bool gif__lzw(image_reader_t *reader, uint8_t minimum_code_size, gif_output_t *output) {
    if (minimum_code_size < 2 || minimum_code_size > 8) return false;
    size_t compressed_start = reader->offset, compressed_size = 0;
    uint8_t count = 1;
    while (image__reader_read_u8(reader, &count) && count != 0) {
        if (!image__reader_skip(reader, count)) return false;
        compressed_size += count;
    }
    if (count != 0) return false;
    uint8_t *compressed = memory__malloc(compressed_size);
    uint16_t *prefix = memory__malloc(GIF_DICTIONARY_SIZE * sizeof(*prefix));
    uint8_t *suffix = memory__malloc(GIF_DICTIONARY_SIZE);
    uint8_t *stack = memory__malloc(GIF_DICTIONARY_SIZE + 1);
    if (compressed == NULL || prefix == NULL || suffix == NULL || stack == NULL) {
        memory__free(stack);
        memory__free(suffix);
        memory__free(prefix);
        memory__free(compressed);
        return false;
    }
    image_reader_t blocks = {.data = reader->data, .size = reader->size, .offset = compressed_start};
    size_t copied = 0;
    while (image__reader_read_u8(&blocks, &count) && count != 0) {
        if (!image__reader_read(&blocks, compressed + copied, count)) break;
        copied += count;
    }
    int clear = 1 << minimum_code_size, end = clear + 1, available = clear + 2;
    int code_size = minimum_code_size + 1, old_code = -1;
    uint8_t first = 0;
    uint32_t datum = 0;
    int bits = 0;
    size_t input = 0;
    bool ok = true, saw_end = false;
    while (input < copied || bits >= code_size) {
        while (bits < code_size && input < copied) {
            datum |= (uint32_t)compressed[input++] << bits;
            bits += 8;
        }
        if (bits < code_size) break;
        int code = datum & ((1 << code_size) - 1);
        datum >>= code_size;
        bits -= code_size;
        if (code == clear) {
            available = clear + 2;
            code_size = minimum_code_size + 1;
            old_code = -1;
            continue;
        }
        if (code == end) {
            saw_end = true;
            break;
        }
        if (code < 0 || code >= GIF_DICTIONARY_SIZE || code > available) {
            ok = false;
            break;
        }
        if (old_code < 0) {
            if (code >= clear) {
                ok = false;
                break;
            }
            first = (uint8_t)code;
            gif__emit(output, first);
            old_code = code;
            continue;
        }
        int current = code;
        size_t stack_size = 0;
        if (code == available) {
            stack[stack_size++] = first;
            code = old_code;
        }
        while (code >= clear && stack_size < GIF_DICTIONARY_SIZE) {
            stack[stack_size++] = suffix[code];
            code = prefix[code];
        }
        if (code >= clear || stack_size >= GIF_DICTIONARY_SIZE) {
            ok = false;
            break;
        }
        first = (uint8_t)code;
        stack[stack_size++] = first;
        while (stack_size > 0) gif__emit(output, stack[--stack_size]);
        if (available < GIF_DICTIONARY_SIZE) {
            prefix[available] = (uint16_t)old_code;
            suffix[available] = first;
            available++;
            if (available == (1 << code_size) && code_size < 12) code_size++;
        }
        old_code = current;
    }
    memory__free(stack);
    memory__free(suffix);
    memory__free(prefix);
    memory__free(compressed);
    return ok && saw_end && !output->invalid_output &&
           output->pixel_index == (size_t)output->frame_width * output->frame_height;
}

static void gif__fill_canvas(bruce_gif_t *gif) {
    size_t count = (size_t)gif->canvas_width * gif->canvas_height;
    for (size_t i = 0; i < count; ++i) gif->canvas[i] = gif->options.background;
}

static void gif__restore_previous(bruce_gif_t *gif) {
    if (gif->previous_disposal == 3 && gif->restore_canvas != NULL) {
        memcpy(
            gif->canvas,
            gif->restore_canvas,
            (size_t)gif->canvas_width * gif->canvas_height * sizeof(*gif->canvas)
        );
    } else if (gif->previous_disposal == 2) {
        uint32_t x0 = ((uint32_t)gif->previous_left * gif->canvas_width) / gif->source_width;
        uint32_t y0 = ((uint32_t)gif->previous_top * gif->canvas_height) / gif->source_height;
        uint32_t x1 = ((uint32_t)(gif->previous_left + gif->previous_width) * gif->canvas_width +
                       gif->source_width - 1u) /
                      gif->source_width;
        uint32_t y1 = ((uint32_t)(gif->previous_top + gif->previous_height) * gif->canvas_height +
                       gif->source_height - 1u) /
                      gif->source_height;
        if (x1 > gif->canvas_width) x1 = gif->canvas_width;
        if (y1 > gif->canvas_height) y1 = gif->canvas_height;
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x)
                gif->canvas[(size_t)y * gif->canvas_width + x] = gif->options.background;
    }
    gif->previous_disposal = 0;
}

static bruce_result_t gif__decode_next(bruce_gif_t *gif, bool *out_trailer) {
    int transparent_index = -1;
    uint8_t disposal = 0;
    uint32_t delay_ms = 0;
    *out_trailer = false;
    gif__restore_previous(gif);
    for (;;) {
        uint8_t marker;
        if (!image__reader_read_u8(&gif->reader, &marker)) return BRUCE_ERR_IO;
        if (marker == 0x3b) {
            *out_trailer = true;
            return BRUCE_OK;
        }
        if (marker == 0x21) {
            uint8_t label;
            if (!image__reader_read_u8(&gif->reader, &label)) return BRUCE_ERR_IO;
            if (label == 0xf9) {
                uint8_t block_size, control[4], terminator;
                if (!image__reader_read_u8(&gif->reader, &block_size) || block_size != 4 ||
                    !image__reader_read(&gif->reader, control, sizeof(control)) ||
                    !image__reader_read_u8(&gif->reader, &terminator) || terminator != 0)
                    return BRUCE_ERR_IO;
                disposal = (control[0] >> 2) & 7;
                transparent_index = (control[0] & 1) != 0 ? control[3] : -1;
                delay_ms = (uint32_t)(control[1] | ((uint16_t)control[2] << 8)) * 10u;
            } else if (!gif__skip_blocks(&gif->reader)) return BRUCE_ERR_IO;
            continue;
        }
        if (marker != 0x2c) return BRUCE_ERR_IO;
        gif_output_t output = {
            .pixels = gif->canvas,
            .canvas_width = gif->canvas_width,
            .canvas_height = gif->canvas_height,
            .source_width = gif->source_width,
            .source_height = gif->source_height,
            .transparent_index = transparent_index,
        };
        uint8_t image_packed;
        if (!image__reader_read_u16(&gif->reader, &output.left) ||
            !image__reader_read_u16(&gif->reader, &output.top) ||
            !image__reader_read_u16(&gif->reader, &output.frame_width) ||
            !image__reader_read_u16(&gif->reader, &output.frame_height) ||
            !image__reader_read_u8(&gif->reader, &image_packed) || output.frame_width == 0 ||
            output.frame_height == 0)
            return BRUCE_ERR_IO;
        output.interlaced = (image_packed & 0x40) != 0;
        if ((image_packed & 0x80) != 0) {
            output.palette_count = (size_t)1 << ((image_packed & 7) + 1);
            if (!gif__palette(&gif->reader, output.palette, output.palette_count)) return BRUCE_ERR_IO;
        } else {
            if (gif->global_palette_count == 0) return BRUCE_ERR_IO;
            output.palette_count = gif->global_palette_count;
            memcpy(output.palette, gif->global_palette, sizeof(gif->global_palette));
        }
        if (disposal == 3) {
            if (gif->restore_canvas == NULL) {
                gif->restore_canvas = memory__malloc(
                    (size_t)gif->canvas_width * gif->canvas_height * sizeof(*gif->restore_canvas)
                );
                if (gif->restore_canvas == NULL) return BRUCE_ERR_NO_MEMORY;
            }
            memcpy(
                gif->restore_canvas,
                gif->canvas,
                (size_t)gif->canvas_width * gif->canvas_height * sizeof(*gif->canvas)
            );
        }
        uint8_t code_size;
        if (!image__reader_read_u8(&gif->reader, &code_size) || !gif__lzw(&gif->reader, code_size, &output))
            return BRUCE_ERR_IO;
        gif->previous_left = output.left;
        gif->previous_top = output.top;
        gif->previous_width = output.frame_width;
        gif->previous_height = output.frame_height;
        gif->previous_disposal = disposal;
        gif->delay_ms = delay_ms;
        return BRUCE_OK;
    }
}

bruce_result_t gif__open_memory(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options,
    const void *backing, bruce_gif_t **out_gif
) {
    if (data == NULL || size < 13 || options == NULL || out_gif == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_gif = NULL;
    bruce_gif_t *gif = memory__malloc(sizeof(*gif));
    if (gif == NULL) return BRUCE_ERR_NO_MEMORY;
    *gif = (bruce_gif_t){
        .data = data,
        .size = size,
        .reader = {.data = data, .size = size},
        .options = *options,
    };
    uint8_t header[6], packed, background_index, aspect;
    if (!image__reader_read(&gif->reader, header, sizeof(header)) ||
        (memcmp(header, "GIF87a", 6) != 0 && memcmp(header, "GIF89a", 6) != 0) ||
        !image__reader_read_u16(&gif->reader, &gif->source_width) ||
        !image__reader_read_u16(&gif->reader, &gif->source_height) ||
        !image__reader_read_u8(&gif->reader, &packed) ||
        !image__reader_read_u8(&gif->reader, &background_index) ||
        !image__reader_read_u8(&gif->reader, &aspect) || gif->source_width == 0 || gif->source_height == 0) {
        image__gif_close(gif);
        return BRUCE_ERR_IO;
    }
    (void)background_index;
    (void)aspect;
    if ((packed & 0x80) != 0) {
        gif->global_palette_count = (size_t)1 << ((packed & 7) + 1);
        if (!gif__palette(&gif->reader, gif->global_palette, gif->global_palette_count)) {
            image__gif_close(gif);
            return BRUCE_ERR_IO;
        }
    }
    gif->frames_offset = gif->reader.offset;
    image__fit_size(
        gif->source_width, gif->source_height, options->fit, &gif->canvas_width, &gif->canvas_height
    );
    if (gif->canvas_width == 0 || gif->canvas_height == 0 ||
        (size_t)gif->canvas_width > SIZE_MAX / ((size_t)gif->canvas_height * sizeof(*gif->canvas))) {
        image__gif_close(gif);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    gif->canvas = memory__malloc((size_t)gif->canvas_width * gif->canvas_height * sizeof(*gif->canvas));
    if (gif->canvas == NULL) {
        image__gif_close(gif);
        return BRUCE_ERR_NO_MEMORY;
    }
    gif__fill_canvas(gif);
    bool trailer = false;
    bruce_result_t result = gif__decode_next(gif, &trailer);
    if (result != BRUCE_OK || trailer) {
        image__gif_close(gif);
        return result != BRUCE_OK ? result : BRUCE_ERR_IO;
    }
    if (backing != NULL) gif->backing = backing;
    *out_gif = gif;
    return BRUCE_OK;
}

bruce_result_t image__gif_draw(bruce_gif_t *gif, uint32_t *out_delay_ms) {
    if (gif == NULL || gif->canvas == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t result =
        image__draw_pixels(gif->canvas, gif->canvas_width, gif->canvas_height, &gif->options);
    if (result == BRUCE_OK && out_delay_ms != NULL) *out_delay_ms = gif->delay_ms;
    return result;
}

bruce_result_t image__gif_increment(bruce_gif_t *gif, bool *out_looped) {
    if (gif == NULL || gif->canvas == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bool trailer = false;
    bruce_result_t result = gif__decode_next(gif, &trailer);
    if (result != BRUCE_OK) return result;
    bool looped = trailer;
    if (looped) {
        gif->reader.offset = gif->frames_offset;
        gif->previous_disposal = 0;
        gif__fill_canvas(gif);
        result = gif__decode_next(gif, &trailer);
        if (result != BRUCE_OK || trailer) return result != BRUCE_OK ? result : BRUCE_ERR_IO;
    }
    if (out_looped != NULL) *out_looped = looped;
    return BRUCE_OK;
}

void image__gif_close(bruce_gif_t *gif) {
    if (gif == NULL) return;
    memory__free(gif->restore_canvas);
    memory__free(gif->canvas);
    if (gif->backing != NULL) (void)memory__external_free(gif->backing);
    memory__free(gif);
}

bruce_result_t image__decode_gif(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    bruce_gif_t *gif = NULL;
    bruce_result_t result = gif__open_memory(data, size, options, NULL, &gif);
    if (result != BRUCE_OK) return result;
    bitmap->pixels = gif->canvas;
    bitmap->width = gif->canvas_width;
    bitmap->height = gif->canvas_height;
    bitmap->source_width = gif->source_width;
    bitmap->source_height = gif->source_height;
    bitmap->format = BRUCE_IMAGE_FORMAT_GIF;
    gif->canvas = NULL;
    image__gif_close(gif);
    return BRUCE_OK;
}
