#include "core_sdk/image.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "jpeg_decoder.h"
#include "png.h"

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

#define IMAGE_FILE_SIZE_MAX (4u * 1024u * 1024u)
#define GIF_DICTIONARY_SIZE 4096

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} image_reader_t;

static bool image__read(image_reader_t *reader, void *out, size_t size)
{
    if (size > reader->size - reader->offset) return false;
    if (out != NULL) memcpy(out, reader->data + reader->offset, size);
    reader->offset += size;
    return true;
}

static bool image__skip(image_reader_t *reader, size_t size)
{
    return image__read(reader, NULL, size);
}

static bool image__read_u8(image_reader_t *reader, uint8_t *out)
{
    return image__read(reader, out, 1);
}

static bool image__read_u16(image_reader_t *reader, uint16_t *out)
{
    uint8_t bytes[2];
    if (!image__read(reader, bytes, sizeof(bytes))) return false;
    *out = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    return true;
}

static void image__fit_size(uint32_t source_width, uint32_t source_height, bool fit,
                            uint16_t *out_width, uint16_t *out_height)
{
    uint32_t width = source_width;
    uint32_t height = source_height;
    uint32_t viewport_width = (uint32_t)display__width();
    uint32_t viewport_height = (uint32_t)display__height();

    if (fit && viewport_width > 0 && viewport_height > 0 &&
        (width > viewport_width || height > viewport_height)) {
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

static bruce_result_t image__draw_pixels(const uint16_t *pixels, uint16_t width, uint16_t height,
                                         const bruce_image_draw_options_t *options)
{
    if (width > INT16_MAX || height > INT16_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    int x = options->x;
    int y = options->y;
    if (options->center) {
        x += (display__width() - width) / 2;
        y += (display__height() - height) / 2;
    }
    return display__draw_rgb_bitmap((int16_t)x, (int16_t)y, pixels, (int16_t)width, (int16_t)height);
}

static bruce_result_t image__draw_scaled_pixels(const uint16_t *pixels, uint16_t width, uint16_t height,
                                                const bruce_image_draw_options_t *options)
{
    uint16_t out_width;
    uint16_t out_height;
    image__fit_size(width, height, options->fit, &out_width, &out_height);
    if (out_width == width && out_height == height) {
        return image__draw_pixels(pixels, width, height, options);
    }
    if (out_width == 0 || out_height == 0 ||
        (size_t)out_width > SIZE_MAX / ((size_t)out_height * sizeof(uint16_t))) {
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
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

static bruce_result_t image__decode_jpeg(const uint8_t *data, size_t size,
                                         const bruce_image_draw_options_t *options,
                                         bruce_image_info_t *info)
{
    if (size > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    esp_jpeg_image_cfg_t config = {
        .indata = (uint8_t *)data,
        .indata_size = (uint32_t)size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t output;
    if (esp_jpeg_get_image_info(&config, &output) != ESP_OK) return BRUCE_ERR_IO;
    uint16_t source_width = output.width;
    uint16_t source_height = output.height;

    if (options->fit) {
        while ((output.width > display__width() || output.height > display__height()) &&
               config.out_scale < JPEG_IMAGE_SCALE_1_8) {
            config.out_scale++;
            if (esp_jpeg_get_image_info(&config, &output) != ESP_OK) return BRUCE_ERR_IO;
        }
    }
    if (output.width == 0 || output.height == 0 ||
        (size_t)output.width > SIZE_MAX / ((size_t)output.height * sizeof(uint16_t))) {
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    size_t output_size = (size_t)output.width * output.height * sizeof(uint16_t);
    if (output_size > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;

    uint8_t *pixels = memory__malloc(output_size);
    if (pixels == NULL) return BRUCE_ERR_NO_MEMORY;
    config.outbuf = pixels;
    config.outbuf_size = output_size;
    bruce_result_t result = BRUCE_ERR_IO;
    if (esp_jpeg_decode(&config, &output) == ESP_OK) {
        result = image__draw_scaled_pixels((const uint16_t *)pixels, output.width, output.height, options);
        if (info != NULL) {
            info->format = BRUCE_IMAGE_FORMAT_JPEG;
            info->width = source_width;
            info->height = source_height;
        }
    }
    memory__free(pixels);
    return result;
}

static void image__png_read(png_structp png, png_bytep out, size_t count)
{
    image_reader_t *reader = png_get_io_ptr(png);
    if (!image__read(reader, out, count)) png_error(png, "truncated PNG");
}

typedef struct {
    uint8_t *row;
    uint16_t *pixels;
} image_png_allocations_t;

static bruce_result_t image__decode_png(const uint8_t *data, size_t size,
                                        const bruce_image_draw_options_t *options,
                                        bruce_image_info_t *info)
{
    image_reader_t reader = {.data = data, .size = size};
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) return BRUCE_ERR_NO_MEMORY;
    png_infop png_info = png_create_info_struct(png);
    if (png_info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);
        return BRUCE_ERR_NO_MEMORY;
    }

    image_png_allocations_t *allocations = calloc(1, sizeof(*allocations));
    if (allocations == NULL) {
        png_destroy_read_struct(&png, &png_info, NULL);
        return BRUCE_ERR_NO_MEMORY;
    }
    volatile bruce_result_t result = BRUCE_ERR_IO;
    if (setjmp(png_jmpbuf(png)) != 0) goto cleanup;
    png_set_read_fn(png, &reader, image__png_read);
    png_read_info(png, png_info);

    png_uint_32 source_width = png_get_image_width(png, png_info);
    png_uint_32 source_height = png_get_image_height(png, png_info);
    int color_type = png_get_color_type(png, png_info);
    int bit_depth = png_get_bit_depth(png, png_info);
    if (source_width == 0 || source_height == 0 || source_width > UINT16_MAX || source_height > UINT16_MAX) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    if (png_get_interlace_type(png, png_info) != PNG_INTERLACE_NONE) {
        result = BRUCE_ERR_UNSUPPORTED;
        goto cleanup;
    }

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, png_info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !png_get_valid(png, png_info, PNG_INFO_tRNS)) {
        png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    }
    png_read_update_info(png, png_info);

    uint16_t out_width;
    uint16_t out_height;
    image__fit_size(source_width, source_height, options->fit, &out_width, &out_height);
    if (out_width == 0 || out_height == 0 ||
        (size_t)out_width > SIZE_MAX / (sizeof(uint16_t) * out_height)) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    size_t row_size = png_get_rowbytes(png, png_info);
    allocations->row = memory__malloc(row_size);
    allocations->pixels = memory__malloc((size_t)out_width * out_height * sizeof(*allocations->pixels));
    if (allocations->row == NULL || allocations->pixels == NULL) {
        result = BRUCE_ERR_NO_MEMORY;
        goto cleanup;
    }

    for (png_uint_32 sy = 0; sy < source_height; ++sy) {
        png_read_row(png, allocations->row, NULL);
        uint32_t oy = (uint32_t)(((uint64_t)sy * out_height) / source_height);
        uint16_t *out = allocations->pixels + (size_t)oy * out_width;
        for (png_uint_32 sx = 0; sx < source_width; ++sx) {
            uint32_t ox = (uint32_t)(((uint64_t)sx * out_width) / source_width);
            const uint8_t *rgba = allocations->row + sx * 4;
            uint8_t alpha = rgba[3];
            uint8_t bg_r = (uint8_t)(((options->background >> 11) & 0x1f) * 255 / 31);
            uint8_t bg_g = (uint8_t)(((options->background >> 5) & 0x3f) * 255 / 63);
            uint8_t bg_b = (uint8_t)((options->background & 0x1f) * 255 / 31);
            uint8_t r = (uint8_t)((rgba[0] * alpha + bg_r * (255 - alpha)) / 255);
            uint8_t g = (uint8_t)((rgba[1] * alpha + bg_g * (255 - alpha)) / 255);
            uint8_t b = (uint8_t)((rgba[2] * alpha + bg_b * (255 - alpha)) / 255);
            out[ox] = display__color565(r, g, b);
        }
    }
    png_read_end(png, NULL);
    result = image__draw_pixels(allocations->pixels, out_width, out_height, options);
    if (result == BRUCE_OK && info != NULL) {
        info->format = BRUCE_IMAGE_FORMAT_PNG;
        info->width = (uint16_t)source_width;
        info->height = (uint16_t)source_height;
    }

cleanup:
    memory__free(allocations->pixels);
    memory__free(allocations->row);
    free(allocations);
    png_destroy_read_struct(&png, &png_info, NULL);
    return (bruce_result_t)result;
}

static bool image__gif_skip_blocks(image_reader_t *reader)
{
    uint8_t count = 1;
    do {
        if (!image__read_u8(reader, &count) || !image__skip(reader, count)) return false;
    } while (count != 0);
    return true;
}

static bool image__gif_palette(image_reader_t *reader, uint16_t *palette, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint8_t rgb[3];
        if (!image__read(reader, rgb, sizeof(rgb))) return false;
        palette[i] = display__color565(rgb[0], rgb[1], rgb[2]);
    }
    return true;
}

typedef struct {
    uint16_t *pixels;
    uint16_t canvas_width;
    uint16_t canvas_height;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t left;
    uint16_t top;
    uint16_t frame_width;
    uint16_t frame_height;
    uint16_t palette[256];
    int transparent_index;
    bool interlaced;
    bool too_many_pixels;
    size_t pixel_index;
} gif_output_t;

static uint16_t image__gif_interlaced_row(uint16_t row, uint16_t height)
{
    static const uint8_t starts[] = {0, 4, 2, 1};
    static const uint8_t steps[] = {8, 8, 4, 2};
    uint16_t index = 0;
    for (size_t pass = 0; pass < 4; ++pass) {
        for (uint32_t y = starts[pass]; y < height; y += steps[pass]) {
            if (index++ == row) return y;
        }
    }
    return row;
}

static void image__gif_emit(gif_output_t *output, uint8_t color)
{
    if (output->pixel_index >= (size_t)output->frame_width * output->frame_height) {
        output->too_many_pixels = true;
        return;
    }
    uint16_t x = (uint16_t)(output->pixel_index % output->frame_width);
    uint16_t y = (uint16_t)(output->pixel_index / output->frame_width);
    output->pixel_index++;
    if (output->interlaced) y = image__gif_interlaced_row(y, output->frame_height);
    if (color == output->transparent_index) return;

    uint32_t source_x = output->left + x;
    uint32_t source_y = output->top + y;
    if (source_x >= output->source_width || source_y >= output->source_height) return;
    uint32_t out_x = (uint32_t)(((uint64_t)source_x * output->canvas_width) / output->source_width);
    uint32_t out_y = (uint32_t)(((uint64_t)source_y * output->canvas_height) / output->source_height);
    output->pixels[(size_t)out_y * output->canvas_width + out_x] = output->palette[color];
}

static bool image__gif_lzw(image_reader_t *reader, uint8_t minimum_code_size, gif_output_t *output)
{
    if (minimum_code_size < 2 || minimum_code_size > 8) return false;
    size_t compressed_start = reader->offset;
    size_t compressed_size = 0;
    uint8_t count = 1;
    while (image__read_u8(reader, &count) && count != 0) {
        if (!image__skip(reader, count)) return false;
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
    while (image__read_u8(&blocks, &count) && count != 0) {
        if (!image__read(&blocks, compressed + copied, count)) break;
        copied += count;
    }

    int clear = 1 << minimum_code_size;
    int end = clear + 1;
    int available = clear + 2;
    int code_size = minimum_code_size + 1;
    int old_code = -1;
    uint8_t first = 0;
    uint32_t datum = 0;
    int bits = 0;
    size_t input = 0;
    bool ok = true;
    bool saw_end = false;

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
            if (code >= clear) { ok = false; break; }
            first = (uint8_t)code;
            image__gif_emit(output, first);
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
        if (code >= clear || stack_size >= GIF_DICTIONARY_SIZE) { ok = false; break; }
        first = (uint8_t)code;
        stack[stack_size++] = first;
        while (stack_size > 0) image__gif_emit(output, stack[--stack_size]);

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

static bruce_result_t image__decode_gif(const uint8_t *data, size_t size,
                                        const bruce_image_draw_options_t *options,
                                        bruce_image_info_t *info)
{
    image_reader_t reader = {.data = data, .size = size};
    uint8_t header[6];
    uint16_t source_width;
    uint16_t source_height;
    uint8_t packed;
    uint8_t background_index;
    uint8_t aspect;
    if (!image__read(&reader, header, sizeof(header)) ||
        (memcmp(header, "GIF87a", 6) != 0 && memcmp(header, "GIF89a", 6) != 0) ||
        !image__read_u16(&reader, &source_width) || !image__read_u16(&reader, &source_height) ||
        !image__read_u8(&reader, &packed) || !image__read_u8(&reader, &background_index) ||
        !image__read_u8(&reader, &aspect) || source_width == 0 || source_height == 0) {
        return BRUCE_ERR_IO;
    }
    (void)aspect;

    uint16_t global_palette[256] = {0};
    size_t global_count = 0;
    if ((packed & 0x80) != 0) {
        global_count = (size_t)1 << ((packed & 0x07) + 1);
        if (!image__gif_palette(&reader, global_palette, global_count)) return BRUCE_ERR_IO;
    }

    int transparent_index = -1;
    for (;;) {
        uint8_t marker;
        if (!image__read_u8(&reader, &marker)) return BRUCE_ERR_IO;
        if (marker == 0x3b) return BRUCE_ERR_IO;
        if (marker == 0x21) {
            uint8_t label;
            if (!image__read_u8(&reader, &label)) return BRUCE_ERR_IO;
            if (label == 0xf9) {
                uint8_t block_size;
                uint8_t control[4];
                uint8_t terminator;
                if (!image__read_u8(&reader, &block_size) || block_size != 4 ||
                    !image__read(&reader, control, sizeof(control)) ||
                    !image__read_u8(&reader, &terminator) || terminator != 0) return BRUCE_ERR_IO;
                transparent_index = (control[0] & 1) != 0 ? control[3] : -1;
            } else if (!image__gif_skip_blocks(&reader)) {
                return BRUCE_ERR_IO;
            }
            continue;
        }
        if (marker != 0x2c) return BRUCE_ERR_IO;

        gif_output_t output = {
            .source_width = source_width,
            .source_height = source_height,
            .transparent_index = transparent_index,
        };
        uint8_t image_packed;
        if (!image__read_u16(&reader, &output.left) || !image__read_u16(&reader, &output.top) ||
            !image__read_u16(&reader, &output.frame_width) || !image__read_u16(&reader, &output.frame_height) ||
            !image__read_u8(&reader, &image_packed) || output.frame_width == 0 || output.frame_height == 0) {
            return BRUCE_ERR_IO;
        }
        output.interlaced = (image_packed & 0x40) != 0;
        if ((image_packed & 0x80) != 0) {
            size_t local_count = (size_t)1 << ((image_packed & 0x07) + 1);
            if (!image__gif_palette(&reader, output.palette, local_count)) return BRUCE_ERR_IO;
        } else {
            if (global_count == 0) return BRUCE_ERR_IO;
            memcpy(output.palette, global_palette, sizeof(global_palette));
        }

        image__fit_size(source_width, source_height, options->fit,
                        &output.canvas_width, &output.canvas_height);
        if (output.canvas_width == 0 || output.canvas_height == 0 ||
            (size_t)output.canvas_width > SIZE_MAX / ((size_t)output.canvas_height * sizeof(uint16_t))) {
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
        size_t pixel_count = (size_t)output.canvas_width * output.canvas_height;
        output.pixels = memory__malloc(pixel_count * sizeof(*output.pixels));
        if (output.pixels == NULL) return BRUCE_ERR_NO_MEMORY;
        uint16_t background = options->background;
        for (size_t i = 0; i < pixel_count; ++i) output.pixels[i] = background;

        uint8_t code_size;
        bool decoded = image__read_u8(&reader, &code_size) && image__gif_lzw(&reader, code_size, &output);
        bruce_result_t result = decoded
            ? image__draw_pixels(output.pixels, output.canvas_width, output.canvas_height, options)
            : BRUCE_ERR_IO;
        memory__free(output.pixels);
        if (result == BRUCE_OK && info != NULL) {
            info->format = BRUCE_IMAGE_FORMAT_GIF;
            info->width = source_width;
            info->height = source_height;
        }
        return result;
    }
}

bool image__is_supported_path(const char *path)
{
    if (path == NULL) return false;
    const char *extension = strrchr(path, '.');
    if (extension == NULL) return false;
    return strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0 ||
           strcasecmp(extension, ".png") == 0 || strcasecmp(extension, ".gif") == 0;
}

bruce_result_t image__draw_memory(const void *data, size_t size,
                                  const bruce_image_draw_options_t *options,
                                  bruce_image_info_t *out_info)
{
    if (data == NULL || size < 6) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_image_draw_options_t defaults = {
        .center = false,
        .fit = false,
        .background = BRUCE_COLOR_BLACK,
    };
    if (options == NULL) options = &defaults;
    const uint8_t *bytes = data;
    if (bytes[0] == 0xff && bytes[1] == 0xd8) {
        return image__decode_jpeg(bytes, size, options, out_info);
    }
    static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (size >= sizeof(png_signature) && memcmp(bytes, png_signature, sizeof(png_signature)) == 0) {
        return image__decode_png(bytes, size, options, out_info);
    }
    if (memcmp(bytes, "GIF87a", 6) == 0 || memcmp(bytes, "GIF89a", 6) == 0) {
        return image__decode_gif(bytes, size, options, out_info);
    }
    return BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t image__draw_path(const char *path,
                                const bruce_image_draw_options_t *options,
                                bruce_image_info_t *out_info)
{
    if (path == NULL || !image__is_supported_path(path)) return BRUCE_ERR_INVALID_PATH;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t file_size = 0;
    result = storage__seek(file, 0, SEEK_END, &file_size);
    if (result != BRUCE_OK || file_size == 0 || file_size > IMAGE_FILE_SIZE_MAX || file_size > SIZE_MAX) {
        (void)storage__close(file);
        return result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT;
    }
    uint8_t *data = memory__malloc((size_t)file_size);
    if (data == NULL) {
        (void)storage__close(file);
        return BRUCE_ERR_NO_MEMORY;
    }
    result = storage__seek(file, 0, SEEK_SET, NULL);
    size_t total = 0;
    while (result == BRUCE_OK && total < file_size) {
        size_t count = 0;
        result = storage__read(file, data + total, (size_t)file_size - total, &count);
        if (result == BRUCE_OK && count == 0) result = BRUCE_ERR_IO;
        total += count;
    }
    (void)storage__close(file);
    if (result == BRUCE_OK) result = image__draw_memory(data, total, options, out_info);
    memory__free(data);
    return result;
}
