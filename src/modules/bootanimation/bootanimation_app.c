#include "bootanimation_app.h"

#include <string.h>

#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"

#define BOOTANIMATION__TOTAL_MS 7000u
#define BOOTANIMATION__IMAGE_START_MS 2000u
#define BOOTANIMATION__FRAME_MS 20u

static bruce_result_t bootanimation__draw_title(void) {
    const char *title = "Bruce";
    const char *subtitle = "PREDATORY FIRMWARE";
    int width = display__width();
    int height = display__height();
    bruce_display_color_t primary = config__get_pri_color();
    bruce_display_color_t background = config__get_bg_color();

    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    (void)display__fill_screen(background);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(primary);
    (void)display__set_text_size(2);
    (void)display__set_cursor((width - (int)strlen(title) * 16) / 2, 10);
    (void)display__print(title);
    (void)display__set_text_size(1);
    (void)display__set_cursor((width - (int)strlen("BruceIDF") * 8) / 2, 27);
    (void)display__print("BruceIDF");
    (void)display__set_cursor((width - (int)strlen(subtitle) * 8) / 2, height - 18);
    (void)display__print(subtitle);
    return display__present();
}

static bruce_result_t bootanimation__draw_custom_image(const char *path) {
    bruce_image_draw_options_t options = {
        .center = true,
        .fit = true,
        .background = config__get_bg_color(),
    };
    bruce_result_t result = display__begin_frame();
    if (result == BRUCE_OK) result = display__fill_screen(options.background);
    if (result == BRUCE_OK) result = image__draw_path(path, &options, NULL);
    if (result == BRUCE_OK) result = display__present();
    return result;
}

static bruce_result_t bootanimation__draw_native_stage(unsigned int stage) {
    int width = display__width();
    int height = display__height();
    int center_x = width / 2;
    int center_y = (height + 45) / 2;
    bruce_display_color_t primary = config__get_pri_color();
    bruce_display_color_t background = config__get_bg_color();

    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    (void)display__fill_rect(0, 45, width, height - 45, background);
    if (stage == 2) {
        (void)display__draw_rect(2 * width / 3, height / 2, 2, 2, primary);
    } else if (stage == 3) {
        int eye_y = center_y - 8;
        (void)display__fill_triangle(center_x - 34, eye_y - 10, center_x - 5, eye_y, center_x - 30, eye_y + 8, primary);
        (void)display__fill_triangle(center_x + 34, eye_y - 10, center_x + 5, eye_y, center_x + 30, eye_y + 8, primary);
    } else if (stage == 4) {
        int radius = width < height ? width / 3 : height / 3;
        if (radius > 54) radius = 54;
        (void)display__draw_circle(center_x, center_y, (int16_t)radius, primary);
        (void)display__draw_arc(center_x, center_y + 5, (int16_t)(radius - 12), 45, 135, primary);
        (void)display__fill_triangle(
            center_x - radius + 7,
            center_y - radius + 10,
            center_x - 7,
            center_y - 8,
            center_x - radius + 15,
            center_y + 2,
            primary
        );
        (void)display__fill_triangle(
            center_x + radius - 7,
            center_y - radius + 10,
            center_x + 7,
            center_y - 8,
            center_x + radius - 15,
            center_y + 2,
            primary
        );
    }
    return display__present();
}

static bool bootanimation__skip_requested(void) {
    bruce_input_event_t event;
    while (input__poll(&event) == BRUCE_OK) {
        if (event.action == BRUCE_INPUT_PRESS) return true;
    }
    return false;
}

static bruce_result_t bootanimation__finish(bruce_result_t result) {
    bruce_result_t frame_result = display__begin_frame();
    if (frame_result == BRUCE_OK) {
        (void)display__fill_screen(config__get_bg_color());
        frame_result = display__present();
    }
    return result == BRUCE_OK ? frame_result : result;
}

int bootanimation_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (config__get_instant_boot()) return BRUCE_OK;

    bruce_result_t result = process__to_foreground();
    if (result != BRUCE_OK) return result;
    result = bootanimation__draw_title();
    if (result != BRUCE_OK) return result;

    const char *image_path = NULL;
    bool exists = false;
    if (storage__exists("/sdcard/boot.jpg", &exists) == BRUCE_OK && exists)
        image_path = "/sdcard/boot.jpg";
    else if (storage__exists("/sdcard/boot.gif", &exists) == BRUCE_OK && exists)
        image_path = "/sdcard/boot.gif";
    else if (storage__exists("/boot.jpg", &exists) == BRUCE_OK && exists)
        image_path = "/boot.jpg";
    else if (storage__exists("/boot.gif", &exists) == BRUCE_OK && exists)
        image_path = "/boot.gif";

    uint64_t started = runtime__now();
    bool content_drawn = false;
    bool image_attempted = false;
    unsigned int native_stage = 0;
    while (runtime__now() - started < BOOTANIMATION__TOTAL_MS) {
        uint64_t elapsed = runtime__now() - started;
        if (bootanimation__skip_requested()) return bootanimation__finish(BRUCE_OK);
        if (!content_drawn && !image_attempted && elapsed >= BOOTANIMATION__IMAGE_START_MS) {
            image_attempted = true;
            if (image_path != NULL && bootanimation__draw_custom_image(image_path) == BRUCE_OK) {
                content_drawn = true;
            }
        }
        if (!content_drawn) {
            unsigned int next_stage = 0;
            if (elapsed >= 3600u) next_stage = 4;
            else if (elapsed >= 2900u) next_stage = 3;
            else if (elapsed >= 2200u) next_stage = 2;
            else if (elapsed >= 2000u) next_stage = 1;
            if (next_stage != native_stage) {
                result = bootanimation__draw_native_stage(next_stage);
                if (result != BRUCE_OK) return result;
                native_stage = next_stage;
            }
        }
        if (runtime__delay(BOOTANIMATION__FRAME_MS) != BRUCE_OK) {
            return bootanimation__finish(BRUCE_ERR_CANCELLED);
        }
    }
    return bootanimation__finish(BRUCE_OK);
}
