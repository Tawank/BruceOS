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
    int transparent_index;
    bool interlaced, too_many_pixels;
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
    if (output->pixel_index >= (size_t)output->frame_width * output->frame_height) {
        output->too_many_pixels = true;
        return;
    }
    uint16_t x = (uint16_t)(output->pixel_index % output->frame_width);
    uint16_t y = (uint16_t)(output->pixel_index / output->frame_width);
    output->pixel_index++;
    if (output->interlaced) y = gif__interlaced_row(y, output->frame_height);
    if (color == output->transparent_index) return;
    uint32_t source_x = output->left + x, source_y = output->top + y;
    if (source_x >= output->source_width || source_y >= output->source_height) return;
    uint32_t out_x = (uint32_t)(((uint64_t)source_x * output->canvas_width) / output->source_width);
    uint32_t out_y = (uint32_t)(((uint64_t)source_y * output->canvas_height) / output->source_height);
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
    return ok && saw_end && !output->too_many_pixels &&
           output->pixel_index == (size_t)output->frame_width * output->frame_height;
}

bruce_result_t image__decode_gif(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    image_reader_t reader = {.data = data, .size = size};
    uint8_t header[6], packed, background_index, aspect;
    uint16_t source_width, source_height;
    if (!image__reader_read(&reader, header, sizeof(header)) ||
        (memcmp(header, "GIF87a", 6) != 0 && memcmp(header, "GIF89a", 6) != 0) ||
        !image__reader_read_u16(&reader, &source_width) || !image__reader_read_u16(&reader, &source_height) ||
        !image__reader_read_u8(&reader, &packed) || !image__reader_read_u8(&reader, &background_index) ||
        !image__reader_read_u8(&reader, &aspect) || source_width == 0 || source_height == 0)
        return BRUCE_ERR_IO;
    (void)background_index;
    (void)aspect;
    uint16_t global_palette[256] = {0};
    size_t global_count = 0;
    if ((packed & 0x80) != 0) {
        global_count = (size_t)1 << ((packed & 7) + 1);
        if (!gif__palette(&reader, global_palette, global_count)) return BRUCE_ERR_IO;
    }
    int transparent_index = -1;
    for (;;) {
        uint8_t marker;
        if (!image__reader_read_u8(&reader, &marker)) return BRUCE_ERR_IO;
        if (marker == 0x3b) return BRUCE_ERR_IO;
        if (marker == 0x21) {
            uint8_t label;
            if (!image__reader_read_u8(&reader, &label)) return BRUCE_ERR_IO;
            if (label == 0xf9) {
                uint8_t block_size, control[4], terminator;
                if (!image__reader_read_u8(&reader, &block_size) || block_size != 4 ||
                    !image__reader_read(&reader, control, sizeof(control)) ||
                    !image__reader_read_u8(&reader, &terminator) || terminator != 0)
                    return BRUCE_ERR_IO;
                transparent_index = (control[0] & 1) != 0 ? control[3] : -1;
            } else if (!gif__skip_blocks(&reader)) return BRUCE_ERR_IO;
            continue;
        }
        if (marker != 0x2c) return BRUCE_ERR_IO;
        gif_output_t output = {
            .source_width = source_width,
            .source_height = source_height,
            .transparent_index = transparent_index
        };
        uint8_t image_packed;
        if (!image__reader_read_u16(&reader, &output.left) || !image__reader_read_u16(&reader, &output.top) ||
            !image__reader_read_u16(&reader, &output.frame_width) ||
            !image__reader_read_u16(&reader, &output.frame_height) ||
            !image__reader_read_u8(&reader, &image_packed) || output.frame_width == 0 ||
            output.frame_height == 0)
            return BRUCE_ERR_IO;
        output.interlaced = (image_packed & 0x40) != 0;
        if ((image_packed & 0x80) != 0) {
            size_t local_count = (size_t)1 << ((image_packed & 7) + 1);
            if (!gif__palette(&reader, output.palette, local_count)) return BRUCE_ERR_IO;
        } else {
            if (global_count == 0) return BRUCE_ERR_IO;
            memcpy(output.palette, global_palette, sizeof(global_palette));
        }
        image__fit_size(
            source_width, source_height, options->fit, &output.canvas_width, &output.canvas_height
        );
        if (output.canvas_width == 0 || output.canvas_height == 0 ||
            (size_t)output.canvas_width > SIZE_MAX / ((size_t)output.canvas_height * sizeof(uint16_t)))
            return BRUCE_ERR_RESOURCE_LIMIT;
        size_t pixel_count = (size_t)output.canvas_width * output.canvas_height;
        output.pixels = memory__malloc(pixel_count * sizeof(*output.pixels));
        if (output.pixels == NULL) return BRUCE_ERR_NO_MEMORY;
        for (size_t i = 0; i < pixel_count; ++i) output.pixels[i] = options->background;
        uint8_t code_size;
        bool decoded = image__reader_read_u8(&reader, &code_size) && gif__lzw(&reader, code_size, &output);
        if (!decoded) {
            memory__free(output.pixels);
            return BRUCE_ERR_IO;
        }
        bitmap->pixels = output.pixels;
        bitmap->width = output.canvas_width;
        bitmap->height = output.canvas_height;
        bitmap->source_width = source_width;
        bitmap->source_height = source_height;
        bitmap->format = BRUCE_IMAGE_FORMAT_GIF;
        return BRUCE_OK;
    }
}
