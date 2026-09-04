#include "image_loader_app.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"

static bool image_viewer__is_gif(const char *path) {
    const char *extension = path != NULL ? strrchr(path, '.') : NULL;
    return extension != NULL && strcasecmp(extension, ".gif") == 0;
}

/* input__read() surfaces BRUCE_ERR_NOT_FOREGROUND when another process takes
 * over the screen (e.g. alt-tab, or system_menu's overlay); wait here until
 * we're either back in the foreground or the process is gone for good, so
 * that's told apart from a genuine "close the viewer" press. Same pattern as
 * archive_app.c/filemanager_app.c's identical helper. */
static bool image_viewer__resume_after_handoff(void) {
    bruce_process_snapshot_t snapshot;
    bruce_process_id_t self = process__current_id();
    if (self == BRUCE_PROCESS_ID_INVALID || process__snapshot(self, &snapshot) != BRUCE_OK ||
        snapshot.state != BRUCE_PROCESS_BACKGROUND) {
        return false;
    }
    do {
        if (runtime__delay(20) != BRUCE_OK || process__snapshot(self, &snapshot) != BRUCE_OK) return false;
    } while (snapshot.state == BRUCE_PROCESS_BACKGROUND);
    return snapshot.state == BRUCE_PROCESS_FOREGROUND;
}

static bruce_result_t image_viewer__draw_gif(const char *path, const bruce_image_draw_options_t *options) {
    bruce_gif_t *gif = NULL;
    bruce_result_t result = image__gif_open(path, options, &gif, NULL);
    if (result != BRUCE_OK) return result;

    (void)input__flush();
    while (result == BRUCE_OK) {
        uint32_t delay_ms = 0;
        result = display__begin_frame();
        if (result == BRUCE_OK) result = display__fill_screen(options->background);
        if (result == BRUCE_OK) result = image__gif_draw(gif, &delay_ms);
        if (result == BRUCE_OK) result = display__present();
        if (result != BRUCE_OK) break;

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, delay_ms == 0 ? 100 : delay_ms);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            if (image_viewer__resume_after_handoff()) continue;
            break;
        }
        if (input_result == BRUCE_OK && event.action == BRUCE_INPUT_PRESS) break;
        result = image__gif_increment(gif, NULL);
    }
    image__gif_close(gif);
    return result;
}

int image_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Display an image until an input event is received.");
    ap_add_required_arg(parser, "path", "Path to a JPEG, PNG, or GIF image");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    const char *path = ap_get_arg(parser, "path");
    ap_free(parser);
    if (!image__is_supported_path(path)) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_image_draw_options_t options = {
        .center = true,
        .fit = true,
        .background = BRUCE_COLOR_BLACK,
    };
    bruce_result_t result = display__begin_frame();
    if (result == BRUCE_OK) result = display__fill_screen(options.background);
    if (result == BRUCE_OK) result = display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    if (result == BRUCE_OK) result = display__set_text_color(BRUCE_COLOR_WHITE);
    if (result == BRUCE_OK) result = display__set_text_size(2);
    if (result == BRUCE_OK) {
        result = display__draw_centre_string("Loading...", display__width() / 2, (display__height() - 8) / 2);
    }
    if (result == BRUCE_OK) result = display__present();
    if (image_viewer__is_gif(path)) {
        if (result == BRUCE_OK) result = image_viewer__draw_gif(path, &options);
    } else {
        if (result == BRUCE_OK) result = display__begin_frame();
        if (result == BRUCE_OK) result = display__fill_screen(options.background);
        if (result == BRUCE_OK) result = image__draw_path(path, &options, NULL);
        if (result == BRUCE_OK) result = display__present();
    }
    if (result != BRUCE_OK) {
        (void)display__begin_frame();
        (void)display__fill_screen(BRUCE_COLOR_BLACK);
        (void)display__set_text_color(BRUCE_COLOR_RED);
        (void)display__set_cursor(4, 4);
        char message[48];
        snprintf(message, sizeof(message), "Image error: %s", result__to_string(result));
        (void)display__println(message);
        (void)display__present();
    }

    if (!image_viewer__is_gif(path) || result != BRUCE_OK) {
        (void)input__flush();
        for (;;) {
            bruce_input_event_t event;
            bruce_result_t input_result = input__read(&event, 100);
            if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
                if (image_viewer__resume_after_handoff()) continue;
                break;
            }
            if (input_result == BRUCE_OK && event.action == BRUCE_INPUT_PRESS) break;
        }
    }
    return result;
}
