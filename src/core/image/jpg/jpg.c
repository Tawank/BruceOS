#include "jpg.h"

#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jerror.h"
#include "jpeg_decoder.h"
#include "jpeglib.h"

#include "core/image/image.h"
#include "core_sdk/memory.h"

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    volatile bruce_result_t *failure_result;
    const char *volatile *stage;
} image_jpeg_error_t;

typedef struct image_jpeg_backing_store image_jpeg_backing_store_t;

struct image_jpeg_backing_store {
    void (*read)(
        j_common_ptr decoder, image_jpeg_backing_store_t *store, void *buffer, long offset, long count
    );
    void (*write)(
        j_common_ptr decoder, image_jpeg_backing_store_t *store, void *buffer, long offset, long count
    );
    void (*close)(j_common_ptr decoder, image_jpeg_backing_store_t *store);
    bruce_memory_object_t object;
    const uint8_t *data;
    long raw_size;
    uint8_t reserved[48];
};

static const char *const TAG = "bruce_jpeg";

void *__real_jpeg_get_large(j_common_ptr decoder, size_t size);

void *__wrap_jpeg_get_large(j_common_ptr decoder, size_t size) {
    void *allocation = NULL;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= size) {
        allocation = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (allocation == NULL) allocation = __real_jpeg_get_large(decoder, size);
    if (allocation == NULL) {
        ESP_LOGE(
            TAG,
            "large allocation failed: requested=%zu free=%zu largest=%zu",
            size,
            heap_caps_get_free_size(MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
        );
    }
    return allocation;
}

static void image__jpeg_file_error(j_common_ptr decoder, int message_code) {
    decoder->err->msg_code = message_code;
    decoder->err->error_exit(decoder);
}

static void image__jpeg_backing_read(
    j_common_ptr decoder, image_jpeg_backing_store_t *store, void *buffer, long offset, long count
) {
    if (offset < 0 || count < 0 || offset > store->raw_size || count > store->raw_size - offset ||
        offset % (long)sizeof(JBLOCK) != 0 || count % (long)sizeof(JBLOCK) != 0) {
        image__jpeg_file_error(decoder, JERR_TFILE_READ);
    }
    memset(buffer, 0, (size_t)count);
    size_t first_block = (size_t)offset / sizeof(JBLOCK);
    size_t block_count = (size_t)count / sizeof(JBLOCK);
    uint8_t *output = buffer;
    for (size_t i = 0; i < block_count; ++i) {
        memcpy(output + i * sizeof(JBLOCK), store->data + (first_block + i) * sizeof(JCOEF), sizeof(JCOEF));
    }
}

static void image__jpeg_backing_write(
    j_common_ptr decoder, image_jpeg_backing_store_t *store, void *buffer, long offset, long count
) {
    if (offset < 0 || count < 0 || offset > store->raw_size || count > store->raw_size - offset ||
        offset % (long)sizeof(JBLOCK) != 0 || count % (long)sizeof(JBLOCK) != 0) {
        image__jpeg_file_error(decoder, JERR_TFILE_WRITE);
    }
    size_t first_block = (size_t)offset / sizeof(JBLOCK);
    size_t block_count = (size_t)count / sizeof(JBLOCK);
    const uint8_t *input = buffer;
    uint8_t dc_values[128 * sizeof(JCOEF)];
    for (size_t first = 0; first < block_count; first += 128) {
        size_t chunk = block_count - first < 128 ? block_count - first : 128;
        for (size_t i = 0; i < chunk; ++i) {
            memcpy(dc_values + i * sizeof(JCOEF), input + (first + i) * sizeof(JBLOCK), sizeof(JCOEF));
        }
        if (memory__external_write(
                &store->object, (first_block + first) * sizeof(JCOEF), dc_values, chunk * sizeof(JCOEF)
            ) != BRUCE_OK) {
            image__jpeg_file_error(decoder, JERR_TFILE_WRITE);
        }
    }
}

static void image__jpeg_backing_close(j_common_ptr decoder, image_jpeg_backing_store_t *store) {
    (void)decoder;
    if (store->object.handle != 0) (void)memory__external_free(&store->object);
}

void __wrap_jpeg_open_backing_store(j_common_ptr decoder, void *backing_store, long total_size) {
    image_jpeg_backing_store_t *store = backing_store;
    j_decompress_ptr decompressor = (j_decompress_ptr)decoder;
    if (!decoder->is_decompressor || !decompressor->buffered_image || decompressor->scale_denom != 8 ||
        total_size <= 0 || total_size % (long)sizeof(JBLOCK) != 0) {
        image__jpeg_file_error(decoder, JERR_NO_BACKING_STORE);
    }
    memset(&store->object, 0, sizeof(store->object));
    store->data = NULL;
    store->raw_size = total_size;
    size_t compressed_size = (size_t)total_size / DCTSIZE2;
    if (memory__external_alloc(compressed_size, &store->object) != BRUCE_OK ||
        memory__external_map(&store->object, (const void **)&store->data) != BRUCE_OK) {
        if (store->object.handle != 0) (void)memory__external_free(&store->object);
        image__jpeg_file_error(decoder, JERR_OUT_OF_MEMORY);
    }
    store->read = image__jpeg_backing_read;
    store->write = image__jpeg_backing_write;
    store->close = image__jpeg_backing_close;
}

static void image__jpeg_error_exit(j_common_ptr decoder) {
    image_jpeg_error_t *error = (image_jpeg_error_t *)decoder->err;
    *error->failure_result = error->base.msg_code == JERR_OUT_OF_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_IO;
    char message[JMSG_LENGTH_MAX];
    error->base.format_message(decoder, message);
    ESP_LOGE(TAG, "%s: %s (caller=%p)", *error->stage, message, __builtin_return_address(0));
    longjmp(error->jump, 1);
}

static bruce_result_t image__decode_jpeg_baseline(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
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
        (size_t)output.width > SIZE_MAX / ((size_t)output.height * sizeof(uint16_t)))
        return BRUCE_ERR_RESOURCE_LIMIT;
    size_t output_size = (size_t)output.width * output.height * sizeof(uint16_t);
    if (output_size > UINT32_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    uint8_t *pixels = memory__malloc(output_size);
    if (pixels == NULL) return BRUCE_ERR_NO_MEMORY;
    config.outbuf = pixels;
    config.outbuf_size = output_size;
    esp_err_t error = esp_jpeg_decode(&config, &output);
    if (error == ESP_OK) {
        bitmap->pixels = (uint16_t *)pixels;
        bitmap->width = output.width;
        bitmap->height = output.height;
        bitmap->source_width = source_width;
        bitmap->source_height = source_height;
        bitmap->format = BRUCE_IMAGE_FORMAT_JPEG;
        return BRUCE_OK;
    }
    memory__free(pixels);
    return error == ESP_ERR_NO_MEM ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_IO;
}

/* skip_full_quality forces every coefficient array through libjpeg's backing
 * store instead of a single large in-RAM allocation, regardless of what the
 * memory__get_stats() heuristic below would have picked. The backing store
 * already streams through memory__external() (swap flash when no PSRAM is
 * present, see memory.h) in small fixed-size chunks -- see
 * image__jpeg_backing_read()/write() -- so this is the "smaller buffers,
 * more memory__external" fallback for devices too tight on internal RAM to
 * ever satisfy the full-quality check. */
static bruce_result_t image__decode_jpeg_progressive_attempt(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap,
    bool skip_full_quality
) {
    struct jpeg_decompress_struct decoder = {0};
    image_jpeg_error_t error = {0};
    uint16_t *volatile pixels = NULL;
    volatile bruce_result_t failure_result = BRUCE_ERR_IO;
    const char *volatile stage = "create";

    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = image__jpeg_error_exit;
    error.failure_result = &failure_result;
    error.stage = &stage;
    if (setjmp(error.jump) != 0) {
        jpeg_destroy_decompress(&decoder);
        memory__free((uint16_t *)pixels);
        return failure_result;
    }

    jpeg_create_decompress(&decoder);
    decoder.mem->max_memory_to_use = 64u * 1024u;
    jpeg_mem_src(&decoder, data, size);
    stage = "read header";
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK || decoder.image_width > UINT16_MAX ||
        decoder.image_height > UINT16_MAX) {
        jpeg_destroy_decompress(&decoder);
        return BRUCE_ERR_UNSUPPORTED;
    }

    uint16_t source_width = (uint16_t)decoder.image_width;
    uint16_t source_height = (uint16_t)decoder.image_height;
    decoder.out_color_space = JCS_RGB565;
    decoder.dither_mode = JDITHER_NONE;
    decoder.do_block_smoothing = FALSE;
    decoder.scale_num = 1;
    decoder.scale_denom = 1;
    if (options->fit) {
        while ((decoder.image_width / decoder.scale_denom > display__width() ||
                decoder.image_height / decoder.scale_denom > display__height()) &&
               decoder.scale_denom < 8) {
            decoder.scale_denom *= 2;
        }
    }
    size_t coefficient_size = 0;
    size_t largest_component = 0;
    for (int i = 0; i < decoder.num_components; ++i) {
        jpeg_component_info *component = &decoder.comp_info[i];
        size_t blocks = (size_t)component->width_in_blocks * component->height_in_blocks;
        if (blocks > (SIZE_MAX - coefficient_size) / sizeof(JBLOCK)) {
            jpeg_destroy_decompress(&decoder);
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
        size_t component_size = blocks * sizeof(JBLOCK);
        coefficient_size += component_size;
        if (component_size > largest_component) largest_component = component_size;
    }
    bool full_quality = !skip_full_quality && coefficient_size <= decoder.mem->max_memory_to_use;
    if (!skip_full_quality && decoder.progressive_mode && !full_quality &&
        coefficient_size <= LONG_MAX - 64u * 1024u) {
        size_t required = coefficient_size + 64u * 1024u;
        size_t largest_required = largest_component + 32u;
        bruce_memory_stats_t stats;
        full_quality = memory__get_stats(&stats) == BRUCE_OK && stats.psram_free >= required &&
                       stats.psram_largest_block >= largest_required;
        if (!full_quality) {
            /* memory__get_stats()'s internal_free/internal_largest_block come from
             * MALLOC_CAP_INTERNAL, which also counts internal memory (e.g. IRAM used as
             * heap) that isn't usable here. jpeg_get_large() falls back to plain malloc()
             * (see jmemnobs.c), which on this target draws from MALLOC_CAP_8BIT -- the same
             * mask __wrap_jpeg_get_large()'s own out-of-memory log above already uses. Using
             * the wider INTERNAL figures here let this check see room that the actual
             * allocation can't reach, pick full_quality=true, and then fail inside
             * jpeg_start_decompress() instead of taking the backing-store path. */
            full_quality = heap_caps_get_free_size(MALLOC_CAP_8BIT) >= required &&
                           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= largest_required;
        }
    }
    if (decoder.progressive_mode && full_quality && coefficient_size <= LONG_MAX - 64u * 1024u)
        decoder.mem->max_memory_to_use = (long)(coefficient_size + 64u * 1024u);
    if (decoder.progressive_mode && !full_quality && options->fit) { decoder.scale_denom = 8; }
    jpeg_calc_output_dimensions(&decoder);
    if (decoder.output_width == 0 || decoder.output_height == 0 ||
        decoder.output_width > SIZE_MAX / (decoder.output_height * sizeof(uint16_t))) {
        jpeg_destroy_decompress(&decoder);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    bool dc_scan_only = decoder.progressive_mode && !full_quality && decoder.scale_denom == 8;
    bool upscale_output = decoder.progressive_mode && options->fit && decoder.scale_denom == 8;
    decoder.buffered_image = dc_scan_only;
    ESP_LOGI(
        TAG,
        "progressive=%d arithmetic=%d scale=1/%u buffered=%d full_quality=%d color=%d",
        decoder.progressive_mode,
        decoder.arith_code,
        decoder.scale_denom,
        decoder.buffered_image,
        full_quality,
        decoder.out_color_space
    );
    stage = "start decompression";
    if (!jpeg_start_decompress(&decoder)) {
        jpeg_destroy_decompress(&decoder);
        return BRUCE_ERR_IO;
    }
    if (dc_scan_only) {
        int status;
        stage = "consume DC scan";
        do {
            status = jpeg_consume_input(&decoder);
        } while (status != JPEG_SCAN_COMPLETED && status != JPEG_REACHED_EOI);
        stage = "start DC output";
        if (!jpeg_start_output(&decoder, decoder.input_scan_number)) {
            jpeg_destroy_decompress(&decoder);
            return BRUCE_ERR_IO;
        }
    }

    size_t output_size = decoder.output_width * decoder.output_height * sizeof(uint16_t);
    pixels = memory__malloc(output_size);
    if (pixels == NULL) {
        jpeg_destroy_decompress(&decoder);
        return BRUCE_ERR_NO_MEMORY;
    }

    size_t row_size = decoder.output_width * sizeof(uint16_t);
    stage = "read scanlines";
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW row = (JSAMPROW)((uint8_t *)pixels + decoder.output_scanline * row_size);
        if (jpeg_read_scanlines(&decoder, &row, 1) != 1) {
            jpeg_destroy_decompress(&decoder);
            memory__free((uint16_t *)pixels);
            return BRUCE_ERR_IO;
        }
    }
    stage = dc_scan_only ? "finish DC output" : "finish decompression";
    if (dc_scan_only ? !jpeg_finish_output(&decoder) : !jpeg_finish_decompress(&decoder)) {
        jpeg_destroy_decompress(&decoder);
        memory__free((uint16_t *)pixels);
        return BRUCE_ERR_IO;
    }

    uint16_t decoded_width = (uint16_t)decoder.output_width;
    uint16_t decoded_height = (uint16_t)decoder.output_height;
    jpeg_destroy_decompress(&decoder);

    if (upscale_output) {
        uint16_t target_width = display__width();
        uint16_t target_height = (uint16_t)(((uint32_t)source_height * target_width) / source_width);
        if (target_height == 0) target_height = 1;
        if (target_height > display__height()) {
            target_height = display__height();
            target_width = (uint16_t)(((uint32_t)source_width * target_height) / source_height);
            if (target_width == 0) target_width = 1;
        }
        if (target_width > decoded_width || target_height > decoded_height) {
            size_t scaled_size = (size_t)target_width * target_height * sizeof(uint16_t);
            uint16_t *scaled = memory__malloc(scaled_size);
            if (scaled == NULL) {
                memory__free((uint16_t *)pixels);
                return BRUCE_ERR_NO_MEMORY;
            }
            for (uint16_t y = 0; y < target_height; ++y) {
                size_t source_row = (size_t)y * decoded_height / target_height;
                for (uint16_t x = 0; x < target_width; ++x) {
                    size_t source_column = (size_t)x * decoded_width / target_width;
                    scaled[(size_t)y * target_width + x] =
                        ((uint16_t *)pixels)[source_row * decoded_width + source_column];
                }
            }
            memory__free((uint16_t *)pixels);
            pixels = scaled;
            decoded_width = target_width;
            decoded_height = target_height;
        }
    }

    bitmap->pixels = (uint16_t *)pixels;
    bitmap->width = decoded_width;
    bitmap->height = decoded_height;
    bitmap->source_width = source_width;
    bitmap->source_height = source_height;
    bitmap->format = BRUCE_IMAGE_FORMAT_JPEG;
    return BRUCE_OK;
}

/* The first attempt trusts memory__get_stats() to decide whether a
 * single-shot, full-RAM decode fits. That snapshot can go stale by the time
 * jpeg_start_decompress() actually asks for the large coefficient block --
 * header parsing and per-component allocations in between eat into the same
 * heap -- so on devices with only tens of KB free the first attempt can
 * still lose the race and come back BRUCE_ERR_NO_MEMORY. Rather than fail
 * the whole decode, retry once with the backing store forced on: slower,
 * but bounded to small fixed-size buffers instead of one large allocation. */
static bruce_result_t image__decode_jpeg_progressive(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    bruce_result_t result = image__decode_jpeg_progressive_attempt(data, size, options, bitmap, false);
    if (result == BRUCE_ERR_NO_MEMORY) {
        ESP_LOGW(TAG, "full-quality decode ran out of memory, retrying via backing store");
        result = image__decode_jpeg_progressive_attempt(data, size, options, bitmap, true);
    }
    return result;
}

bruce_result_t image__decode_jpeg(
    const uint8_t *data, size_t size, const bruce_image_draw_options_t *options, image_bitmap_t *bitmap
) {
    bruce_result_t result = image__decode_jpeg_baseline(data, size, options, bitmap);
    if (result != BRUCE_ERR_IO) return result;
    return image__decode_jpeg_progressive(data, size, options, bitmap);
}
