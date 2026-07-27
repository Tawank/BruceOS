#include "image_test.h"

#include <stdio.h>

#include "core/display/display.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"

bool selftest__run_image_decode_case(void) {
    static const uint8_t red_gif[] = {
        'G', 'I',  'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0xff, 0,    0, 0,    0,
        0,   0x2c, 0,   0,   0,   0,   1, 0, 1, 0, 0,    2, 2, 0x44, 0x01, 0, 0x3b,
    };
    static const uint8_t invalid[] = {'n', 'o', 't', 'i', 'm', 'g'};
    bruce_image_draw_options_t options = {.background = BRUCE_COLOR_BLACK};
    bruce_image_info_t info;

    if (!image__is_supported_path("/photo.JPEG") || !image__is_supported_path("/icon.png") ||
        !image__is_supported_path("/anim.GIF") || image__is_supported_path("/notes.txt")) {
        printf("[selftest] image/decode: FAIL, extension matching\n");
        return false;
    }
    if (image__draw_memory(invalid, sizeof(invalid), &options, NULL) != BRUCE_ERR_UNSUPPORTED) {
        printf("[selftest] image/decode: FAIL, unsupported signature\n");
        return false;
    }
    if (display__begin_frame() != BRUCE_OK) {
        printf("[selftest] image/decode: FAIL, begin frame\n");
        return false;
    }
    if (display__fill_screen(BRUCE_COLOR_BLACK) != BRUCE_OK ||
        image__draw_memory(red_gif, sizeof(red_gif), &options, &info) != BRUCE_OK) {
        (void)display__present();
        printf("[selftest] image/decode: FAIL, GIF decode\n");
        return false;
    }
    bruce_display_color_t pixel = 0;
    bool valid = info.format == BRUCE_IMAGE_FORMAT_GIF && info.width == 1 && info.height == 1 &&
                 display__test_read_pixel(0, 0, &pixel) == BRUCE_OK && pixel == BRUCE_COLOR_RED;
    (void)display__present();
    if (!valid) printf("[selftest] image/decode: FAIL, GIF output\n");
    return valid;
}
