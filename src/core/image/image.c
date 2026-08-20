#include "core_sdk/image.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core/image/gif/gif.h"
#include "core/image/jpg/jpg.h"
#include "core/image/png/png.h"
#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

#define IMAGE_FILE_SIZE_MAX (1u * 1024u * 1024u)
#define IMAGE_READ_CHUNK_SIZE 4096u

typedef enum {
    IMAGE_FORMAT_UNKNOWN,
    IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_GIF,
} image_format_t;

static image_format_t image__detect_format(const uint8_t *bytes, size_t size) {
    static const uint8_t png_signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8) return IMAGE_FORMAT_JPEG;
    if (size >= sizeof(png_signature) && memcmp(bytes, png_signature, sizeof(png_signature)) == 0)
        return IMAGE_FORMAT_PNG;
    if (size >= 6 && (memcmp(bytes, "GIF87a", 6) == 0 || memcmp(bytes, "GIF89a", 6) == 0))
        return IMAGE_FORMAT_GIF;
    return IMAGE_FORMAT_UNKNOWN;
}

bool image__is_supported_path(const char *path) {
    if (path == NULL) return false;
    const char *extension = strrchr(path, '.');
    if (extension == NULL) return false;
    return strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0 ||
           strcasecmp(extension, ".png") == 0 || strcasecmp(extension, ".gif") == 0;
}

static bruce_result_t image__decode_memory(
    const uint8_t *bytes, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    if (bytes == NULL || size < 6) return BRUCE_ERR_INVALID_ARGUMENT;
    switch (image__detect_format(bytes, size)) {
        case IMAGE_FORMAT_JPEG: return image__decode_jpeg(bytes, size, options, bitmap);
        case IMAGE_FORMAT_PNG: return image__decode_png(bytes, size, options, bitmap);
        case IMAGE_FORMAT_GIF: return image__decode_gif(bytes, size, options, bitmap);
        case IMAGE_FORMAT_UNKNOWN: return BRUCE_ERR_UNSUPPORTED;
    }
    return BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t image__get_bitmap_from_memory(
    const void *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *out_bitmap
) {
    if (data == NULL || size < 6 || out_bitmap == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_bitmap = (image_bitmap_t){0};
    bruce_image_draw_options_t defaults = {.center = false, .fit = false, .background = BRUCE_COLOR_BLACK};
    if (options == NULL) options = &defaults;
    return image__decode_memory(data, size, options, out_bitmap);
}

bruce_result_t image__bitmap_resize(
    const image_bitmap_t *source, uint16_t max_width, uint16_t max_height, image_bitmap_t *out_bitmap
) {
    if (source == NULL || source->pixels == NULL || source->width == 0 || source->height == 0 ||
        max_width == 0 || max_height == 0 || out_bitmap == NULL || out_bitmap == source) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_bitmap = (image_bitmap_t){0};

    uint32_t target_width = source->width;
    uint32_t target_height = source->height;
    if (target_width > max_width || target_height > max_height) {
        if ((uint64_t)max_width * target_height <= (uint64_t)max_height * target_width) {
            target_height = (target_height * max_width) / target_width;
            target_width = max_width;
        } else {
            target_width = (target_width * max_height) / target_height;
            target_height = max_height;
        }
        if (target_width == 0) target_width = 1;
        if (target_height == 0) target_height = 1;
    }
    if (target_width > SIZE_MAX / (target_height * sizeof(uint16_t))) return BRUCE_ERR_RESOURCE_LIMIT;

    size_t row_size = target_width * sizeof(uint16_t);
    bruce_memory_object_t backing = {0};
    bruce_result_t result = memory__external_alloc(row_size * target_height, &backing);
    if (result != BRUCE_OK) return result;
    uint16_t *row = memory__malloc(row_size);
    if (row == NULL) {
        (void)memory__external_free(&backing);
        return BRUCE_ERR_NO_MEMORY;
    }

    for (uint32_t y = 0; result == BRUCE_OK && y < target_height; ++y) {
        uint32_t source_y = (uint32_t)(((uint64_t)y * source->height) / target_height);
        for (uint32_t x = 0; x < target_width; ++x) {
            uint32_t source_x = (uint32_t)(((uint64_t)x * source->width) / target_width);
            row[x] = source->pixels[(size_t)source_y * source->width + source_x];
        }
        result = memory__external_write(&backing, (size_t)y * row_size, row, row_size);
    }
    memory__free(row);

    const void *pixels = NULL;
    if (result == BRUCE_OK) result = memory__external_map(&backing, &pixels);
    if (result != BRUCE_OK) {
        (void)memory__external_free(&backing);
        return result;
    }
    *out_bitmap = (image_bitmap_t){
        .pixels = (uint16_t *)pixels,
        .width = (uint16_t)target_width,
        .height = (uint16_t)target_height,
        .source_width = source->source_width,
        .source_height = source->source_height,
        .format = source->format,
        .backing = backing,
    };
    return BRUCE_OK;
}

bruce_result_t image__draw_bitmap(
    const image_bitmap_t *bitmap, const bruce_image_draw_options_t *options
) {
    if (bitmap == NULL || bitmap->pixels == NULL || bitmap->width == 0 || bitmap->height == 0)
        return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_image_draw_options_t defaults = {.center = false, .fit = false, .background = BRUCE_COLOR_BLACK};
    if (options == NULL) options = &defaults;
    return image__draw_pixels(bitmap->pixels, bitmap->width, bitmap->height, options);
}

static bruce_result_t image__read_file(bruce_file_id_t file, void *data, size_t size) {
    bruce_result_t result = storage__seek(file, 0, SEEK_SET, NULL);
    size_t total = 0;
    while (result == BRUCE_OK && total < size) {
        size_t count = 0;
        result = storage__read(file, (uint8_t *)data + total, size - total, &count);
        if (result == BRUCE_OK && count == 0) result = BRUCE_ERR_IO;
        total += count;
    }
    return result;
}

bruce_result_t image__gif_open(
    const char *path, const bruce_image_draw_options_t *options, bruce_gif_t **out_gif,
    bruce_image_info_t *out_info
) {
    if (path == NULL || out_gif == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_gif = NULL;
    bruce_image_draw_options_t defaults = {.center = false, .fit = false, .background = BRUCE_COLOR_BLACK};
    if (options == NULL) options = &defaults;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    uint64_t file_size = 0;
    result = storage__seek(file, 0, SEEK_END, &file_size);
    if (result != BRUCE_OK || file_size < 13 || file_size > IMAGE_FILE_SIZE_MAX || file_size > SIZE_MAX) {
        (void)storage__close(file);
        return result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT;
    }

    bruce_memory_object_t object = {0};
    result = memory__external_alloc((size_t)file_size, &object);
    uint8_t *chunk = NULL;
    if (result == BRUCE_OK) {
        chunk = memory__malloc(IMAGE_READ_CHUNK_SIZE);
        if (chunk == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    result = result == BRUCE_OK ? storage__seek(file, 0, SEEK_SET, NULL) : result;
    size_t total = 0;
    while (result == BRUCE_OK && total < file_size) {
        size_t wanted = (size_t)file_size - total;
        if (wanted > IMAGE_READ_CHUNK_SIZE) wanted = IMAGE_READ_CHUNK_SIZE;
        size_t count = 0;
        result = storage__read(file, chunk, wanted, &count);
        if (result == BRUCE_OK && count == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) {
            result = memory__external_write(&object, total, chunk, count);
            total += count;
        }
    }
    memory__free(chunk);
    (void)storage__close(file);

    const void *data = NULL;
    if (result == BRUCE_OK) result = memory__external_map(&object, &data);
    if (result == BRUCE_OK && image__detect_format(data, total) != IMAGE_FORMAT_GIF)
        result = BRUCE_ERR_UNSUPPORTED;
    if (result == BRUCE_OK) {
        result = gif__open_memory(data, total, options, &object, out_gif);
        if (result == BRUCE_OK) object = (bruce_memory_object_t){0};
    }
    if (object.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&object);
    if (result == BRUCE_OK && out_info != NULL) {
        out_info->format = BRUCE_IMAGE_FORMAT_GIF;
        out_info->width = (*out_gif)->source_width;
        out_info->height = (*out_gif)->source_height;
    }
    return result;
}

static bruce_result_t image__decode_png_from_file(
    bruce_file_id_t file, const bruce_image_draw_options_t *options, image_bitmap_t *out_bitmap
) {
    bruce_result_t result = storage__seek(file, 0, SEEK_SET, NULL);
    if (result == BRUCE_OK) result = image__decode_png_file(file, options, out_bitmap);
    return result;
}

static bruce_result_t image__decode_jpeg_from_file(
    bruce_file_id_t file, size_t file_size, const bruce_image_draw_options_t *options,
    image_bitmap_t *out_bitmap
) {
    bruce_memory_object_t object = {0};
    bruce_result_t result = memory__external_alloc(file_size, &object);
    if (result != BRUCE_OK) return result;

    uint8_t *chunk = memory__malloc(IMAGE_READ_CHUNK_SIZE);
    if (chunk == NULL) {
        (void)memory__external_free(&object);
        return BRUCE_ERR_NO_MEMORY;
    }

    result = storage__seek(file, 0, SEEK_SET, NULL);
    size_t total = 0;
    while (result == BRUCE_OK && total < file_size) {
        size_t wanted = file_size - total;
        if (wanted > IMAGE_READ_CHUNK_SIZE) wanted = IMAGE_READ_CHUNK_SIZE;
        size_t count = 0;
        result = storage__read(file, chunk, wanted, &count);
        if (result == BRUCE_OK && count == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) {
            result = memory__external_write(&object, total, chunk, count);
            total += count;
        }
    }
    memory__free(chunk);

    const void *data = NULL;
    if (result == BRUCE_OK) result = memory__external_map(&object, &data);
    if (result == BRUCE_OK) result = image__decode_jpeg(data, total, options, out_bitmap);
    bruce_result_t free_result = memory__external_free(&object);
    return result == BRUCE_OK ? free_result : result;
}

static bruce_result_t image__decode_gif_from_file(
    bruce_file_id_t file, size_t file_size, const bruce_image_draw_options_t *options,
    image_bitmap_t *out_bitmap
) {
    bruce_memory_object_t object = {0};
    bruce_result_t result = memory__external_alloc(file_size, &object);
    if (result != BRUCE_OK) return result;

    uint8_t *chunk = memory__malloc(IMAGE_READ_CHUNK_SIZE);
    if (chunk == NULL) {
        (void)memory__external_free(&object);
        return BRUCE_ERR_NO_MEMORY;
    }

    result = storage__seek(file, 0, SEEK_SET, NULL);
    size_t total = 0;
    while (result == BRUCE_OK && total < file_size) {
        size_t wanted = file_size - total;
        if (wanted > IMAGE_READ_CHUNK_SIZE) wanted = IMAGE_READ_CHUNK_SIZE;
        size_t count = 0;
        result = storage__read(file, chunk, wanted, &count);
        if (result == BRUCE_OK && count == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) {
            result = memory__external_write(&object, total, chunk, count);
            total += count;
        }
    }
    memory__free(chunk);

    if (result == BRUCE_OK) {
        const void *data = NULL;
        result = memory__external_map(&object, &data);
        if (result == BRUCE_OK) result = image__decode_gif(data, total, options, out_bitmap);
    }
    bruce_result_t free_result = memory__external_free(&object);
    return result == BRUCE_OK ? free_result : result;
}

bruce_result_t image__get_bitmap_from_file(
    const char *path, const bruce_image_draw_options_t *options, image_bitmap_t *out_bitmap
) {
    if (path == NULL || !image__is_supported_path(path)) return BRUCE_ERR_INVALID_PATH;
    if (out_bitmap == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_bitmap = (image_bitmap_t){0};
    bruce_image_draw_options_t defaults = {.center = false, .fit = false, .background = BRUCE_COLOR_BLACK};
    if (options == NULL) options = &defaults;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    uint64_t file_size = 0;
    result = storage__seek(file, 0, SEEK_END, &file_size);
    if (result != BRUCE_OK || file_size == 0 || file_size > IMAGE_FILE_SIZE_MAX || file_size > SIZE_MAX) {
        (void)storage__close(file);
        return result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT;
    }

    uint8_t signature[8];
    size_t signature_size = file_size < sizeof(signature) ? (size_t)file_size : sizeof(signature);
    result = image__read_file(file, signature, signature_size);
    if (result == BRUCE_OK) {
        switch (image__detect_format(signature, signature_size)) {
            case IMAGE_FORMAT_PNG: result = image__decode_png_from_file(file, options, out_bitmap); break;
            case IMAGE_FORMAT_JPEG:
                result = image__decode_jpeg_from_file(file, (size_t)file_size, options, out_bitmap);
                break;
            case IMAGE_FORMAT_GIF:
                result = image__decode_gif_from_file(file, (size_t)file_size, options, out_bitmap);
                break;
            case IMAGE_FORMAT_UNKNOWN: result = BRUCE_ERR_UNSUPPORTED; break;
        }
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) image__bitmap_release(out_bitmap);
    return result;
}

bruce_result_t
image__draw_path(const char *path, const bruce_image_draw_options_t *options, bruce_image_info_t *out_info) {
    image_bitmap_t bitmap = {0};
    bruce_result_t result = image__get_bitmap_from_file(path, options, &bitmap);
    if (result == BRUCE_OK) result = image__draw_bitmap(&bitmap, options);
    if (result == BRUCE_OK && out_info != NULL) {
        out_info->format = bitmap.format;
        out_info->width = bitmap.source_width;
        out_info->height = bitmap.source_height;
    }
    image__bitmap_release(&bitmap);
    return result;
}
