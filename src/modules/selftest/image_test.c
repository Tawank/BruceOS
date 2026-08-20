#include "image_test.h"

#include <stdio.h>

#include "core/display/display.h"
#include "core/image/gif/gif.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"

bool selftest__run_image_decode_case(void) {
    static const uint8_t red_gif[] = {
        'G', 'I',  'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0xff, 0,    0, 0,    0,
        0,   0x2c, 0,   0,   0,   0,   1, 0, 1, 0, 0,    2, 2, 0x44, 0x01, 0, 0x3b,
    };
    static const uint8_t animated_gif[] = {
        'G', 'I', 'F', '8', '9', 'a',  1,    0, 1, 0, 0x80, 0, 0, 0xff, 0, 0, 0, 0,    0xff, 0x21, 0xf9, 4,
        0,   5,   0,   0,   0,   0x2c, 0,    0, 0, 0, 1,    0, 1, 0,    0, 2, 2, 0x44, 0x01, 0,    0x21, 0xf9,
        4,   0,   5,   0,   0,   0,    0x2c, 0, 0, 0, 0,    1, 0, 1,    0, 0, 2, 2,    0x4c, 0x01, 0,    0x3b,
    };
    static const uint8_t white_progressive_jpeg[] = {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x28, 0x1c, 0x1e, 0x23, 0x1e, 0x19, 0x28, 0x23, 0x21,
        0x23, 0x2d, 0x2b, 0x28, 0x30, 0x3c, 0x64, 0x41, 0x3c, 0x37, 0x37, 0x3c, 0x7b, 0x58, 0x5d, 0x49, 0x64,
        0x91, 0x80, 0x99, 0x96, 0x8f, 0x80, 0x8c, 0x8a, 0xa0, 0xb4, 0xe6, 0xc3, 0xa0, 0xaa, 0xda, 0xad, 0x8a,
        0x8c, 0xc8, 0xff, 0xcb, 0xda, 0xee, 0xf5, 0xff, 0xff, 0xff, 0x9b, 0xc1, 0xff, 0xff, 0xff, 0xfa, 0xff,
        0xe6, 0xfd, 0xff, 0xf8, 0xff, 0xc2, 0x00, 0x0b, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00,
        0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x04, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x67, 0xff,
        0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x05, 0x02, 0x7f, 0xff, 0xc4,
        0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x06, 0x3f, 0x02, 0x7f, 0xff, 0xc4, 0x00,
        0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x3f, 0x21, 0x7f, 0xff, 0xda, 0x00, 0x08,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x10, 0xff, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x08,
        0x01, 0x01, 0x00, 0x01, 0x3f, 0x10, 0x7f, 0xff, 0xd9,
    };
    static const uint8_t invalid[] = {'n', 'o', 't', 'i', 'm', 'g'};
    bruce_image_draw_options_t options = {.background = BRUCE_COLOR_BLACK};
    image_bitmap_t bitmap = {0};

    if (!image__is_supported_path("/photo.JPEG") || !image__is_supported_path("/icon.png") ||
        !image__is_supported_path("/anim.GIF") || image__is_supported_path("/notes.txt")) {
        printf("[selftest] image/decode: FAIL, extension matching\n");
        return false;
    }
    if (image__get_bitmap_from_memory(invalid, sizeof(invalid), &options, &bitmap) != BRUCE_ERR_UNSUPPORTED) {
        printf("[selftest] image/decode: FAIL, unsupported signature\n");
        return false;
    }
    uint16_t source_pixels[] = {BRUCE_COLOR_RED, BRUCE_COLOR_BLUE, BRUCE_COLOR_WHITE, BRUCE_COLOR_BLACK};
    image_bitmap_t source = {
        .pixels = source_pixels,
        .width = 2,
        .height = 2,
        .source_width = 2,
        .source_height = 2,
        .format = BRUCE_IMAGE_FORMAT_PNG,
    };
    image_bitmap_t resized = {0};
    if (image__bitmap_resize(&source, 1, 1, &resized) != BRUCE_OK || resized.width != 1 ||
        resized.height != 1 || resized.pixels == NULL || resized.pixels[0] != BRUCE_COLOR_RED ||
        resized.backing.backend == BRUCE_MEMORY_BACKEND_INVALID) {
        image__bitmap_release(&resized);
        printf("[selftest] image/decode: FAIL, bitmap resize\n");
        return false;
    }
    image__bitmap_release(&resized);
    if (display__begin_frame() != BRUCE_OK) {
        printf("[selftest] image/decode: FAIL, begin frame\n");
        return false;
    }
    if (display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        image__get_bitmap_from_memory(red_gif, sizeof(red_gif), &options, &bitmap) != BRUCE_OK ||
        image__draw_bitmap(&bitmap, &options) != BRUCE_OK) {
        image__bitmap_release(&bitmap);
        (void)display__present();
        printf("[selftest] image/decode: FAIL, GIF decode\n");
        return false;
    }
    bruce_display_color_t pixel = 0;
    bool valid = bitmap.format == BRUCE_IMAGE_FORMAT_GIF && bitmap.source_width == 1 &&
                  bitmap.source_height == 1 &&
                  display__test_read_pixel(0, 0, &pixel) == BRUCE_OK && pixel == BRUCE_COLOR_RED;
    image__bitmap_release(&bitmap);
    bruce_gif_t *gif = NULL;
    uint32_t delay_ms = 0;
    bool looped = false;
    if (valid) {
        valid = gif__open_memory(animated_gif, sizeof(animated_gif), &options, NULL, &gif) == BRUCE_OK &&
                image__gif_draw(gif, &delay_ms) == BRUCE_OK && delay_ms == 50 &&
                image__gif_increment(gif, &looped) == BRUCE_OK && !looped &&
                image__gif_draw(gif, &delay_ms) == BRUCE_OK &&
                display__test_read_pixel(0, 0, &pixel) == BRUCE_OK && pixel == BRUCE_COLOR_BLUE &&
                image__gif_increment(gif, &looped) == BRUCE_OK && looped;
    }
    image__gif_close(gif);
    if (valid) {
        valid = display__fill_screen(BRUCE_COLOR_BLACK) == BRUCE_OK &&
                image__get_bitmap_from_memory(
                    white_progressive_jpeg, sizeof(white_progressive_jpeg), &options, &bitmap
                ) == BRUCE_OK &&
                image__draw_bitmap(&bitmap, &options) == BRUCE_OK && bitmap.format == BRUCE_IMAGE_FORMAT_JPEG &&
                bitmap.source_width == 1 && bitmap.source_height == 1 &&
                display__test_read_pixel(0, 0, &pixel) == BRUCE_OK && pixel == BRUCE_COLOR_WHITE;
    }
    image__bitmap_release(&bitmap);
    (void)display__present();
    if (!valid) printf("[selftest] image/decode: FAIL, decoded output\n");
    return valid;
}
