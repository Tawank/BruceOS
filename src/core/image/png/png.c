#include "png.h"

#include <setjmp.h>

#include <png.h>

#include "core/image/image.h"
#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

typedef struct {
    image_reader_t *memory;
    bruce_file_id_t file;
    bruce_result_t result;
} png_input_t;

static void image__png_read(png_structp png, png_bytep out, size_t count) {
    png_input_t *input = png_get_io_ptr(png);
    if (input->memory != NULL) {
        if (!image__reader_read(input->memory, out, count)) png_error(png, "truncated PNG");
        return;
    }
    size_t total = 0;
    while (total < count) {
        size_t received = 0;
        input->result = storage__read(input->file, out + total, count - total, &received);
        if (input->result != BRUCE_OK || received == 0) {
            if (input->result == BRUCE_OK) input->result = BRUCE_ERR_IO;
            png_error(png, "truncated PNG");
        }
        total += received;
    }
}

typedef struct {
    uint8_t *row;
    uint16_t *pixels;
} png_allocations_t;

static bruce_result_t image__decode_png_input(
    png_input_t *input, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) return BRUCE_ERR_NO_MEMORY;
    png_infop png_info = png_create_info_struct(png);
    if (png_info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);
        return BRUCE_ERR_NO_MEMORY;
    }
    png_allocations_t allocations = {0};
    volatile bruce_result_t result = BRUCE_ERR_IO;
    if (setjmp(png_jmpbuf(png)) != 0) goto cleanup;
    png_set_read_fn(png, input, image__png_read);
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
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !png_get_valid(png, png_info, PNG_INFO_tRNS))
        png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    png_read_update_info(png, png_info);
    uint16_t out_width, out_height;
    image__fit_size(source_width, source_height, options->fit, &out_width, &out_height);
    if (out_width == 0 || out_height == 0 || (size_t)out_width > SIZE_MAX / (sizeof(uint16_t) * out_height)) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    size_t row_size = png_get_rowbytes(png, png_info);
    allocations.row = memory__malloc(row_size);
    allocations.pixels = memory__malloc((size_t)out_width * out_height * sizeof(*allocations.pixels));
    if (allocations.row == NULL || allocations.pixels == NULL) {
        result = BRUCE_ERR_NO_MEMORY;
        goto cleanup;
    }
    for (png_uint_32 sy = 0; sy < source_height; ++sy) {
        png_read_row(png, allocations.row, NULL);
        uint32_t oy = (uint32_t)(((uint64_t)sy * out_height) / source_height);
        uint16_t *out = allocations.pixels + (size_t)oy * out_width;
        for (png_uint_32 sx = 0; sx < source_width; ++sx) {
            uint32_t ox = (uint32_t)(((uint64_t)sx * out_width) / source_width);
            const uint8_t *rgba = allocations.row + sx * 4;
            uint8_t alpha = rgba[3];
            uint8_t bg_r = (uint8_t)(((options->background >> 11) & 0x1f) * 255 / 31);
            uint8_t bg_g = (uint8_t)(((options->background >> 5) & 0x3f) * 255 / 63);
            uint8_t bg_b = (uint8_t)((options->background & 0x1f) * 255 / 31);
            out[ox] = display__color565(
                (uint8_t)((rgba[0] * alpha + bg_r * (255 - alpha)) / 255),
                (uint8_t)((rgba[1] * alpha + bg_g * (255 - alpha)) / 255),
                (uint8_t)((rgba[2] * alpha + bg_b * (255 - alpha)) / 255)
            );
        }
    }
    png_read_end(png, NULL);
    bitmap->pixels = allocations.pixels;
    bitmap->width = out_width;
    bitmap->height = out_height;
    bitmap->source_width = (uint16_t)source_width;
    bitmap->source_height = (uint16_t)source_height;
    bitmap->format = BRUCE_IMAGE_FORMAT_PNG;
    allocations.pixels = NULL;
    result = BRUCE_OK;
cleanup:
    memory__free(allocations.pixels);
    memory__free(allocations.row);
    png_destroy_read_struct(&png, &png_info, NULL);
    return (bruce_result_t)result;
}

bruce_result_t image__decode_png(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    image_reader_t reader = {.data = data, .size = size};
    png_input_t input = {.memory = &reader, .file = BRUCE_FILE_ID_INVALID, .result = BRUCE_OK};
    return image__decode_png_input(&input, options, bitmap);
}

bruce_result_t image__decode_png_file(
    bruce_file_id_t file, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    png_input_t input = {.memory = NULL, .file = file, .result = BRUCE_OK};
    bruce_result_t result = image__decode_png_input(&input, options, bitmap);
    if (result == BRUCE_ERR_IO && input.result != BRUCE_OK) return input.result;
    return result;
}
