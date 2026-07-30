#include "image_loader_app.h"

#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/process.h"

int image_viewer_app_main(int argc, char **argv) {
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
    if (!image__is_supported_path(path)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!app_runner__args_have_background(argc, argv)) {
        bruce_result_t foreground = process__to_foreground();
        if (foreground != BRUCE_OK) return foreground;
    }

    bruce_image_draw_options_t options = {
        .center = true,
        .fit = true,
        .background = BRUCE_COLOR_BLACK,
    };
    bruce_result_t result = display__begin_frame();
    if (result == BRUCE_OK) result = display__fill_screen(options.background);
    if (result == BRUCE_OK) result = image__draw_path(path, &options, NULL);
    if (result == BRUCE_OK) {
        result = display__present();
    } else {
        (void)display__fill_screen(BRUCE_COLOR_BLACK);
        (void)display__set_text_color(BRUCE_COLOR_RED);
        (void)display__set_cursor(4, 4);
        char message[48];
        snprintf(message, sizeof(message), "Image error (%d)", (int)result);
        (void)display__println(message);
        (void)display__flush();
    }

    (void)input__flush();
    for (;;) {
        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) break;
        if (input_result == BRUCE_OK && event.action == BRUCE_INPUT_PRESS) break;
    }
    return result;
}

static bool image_loader__escape_arg(const char *path, char *out, size_t out_size) {
    size_t written = 0;
    for (size_t i = 0; path[i] != '\0'; ++i) {
        if (path[i] == ' ' || path[i] == '\t' || path[i] == '\\' || path[i] == '\'' || path[i] == '"') {
            if (written + 1 >= out_size) return false;
            out[written++] = '\\';
        }
        if (written + 1 >= out_size) return false;
        out[written++] = path[i];
    }
    static const char gui_arg[] = " --gui";
    if (written + sizeof(gui_arg) > out_size) return false;
    memcpy(out + written, gui_arg, sizeof(gui_arg));
    return true;
}

int image_loader__run_path(const char *path, const char *arg, bool in_background) {
    (void)arg;
    if (path == NULL || !image__is_supported_path(path)) return BRUCE_ERR_INVALID_PATH;

    char escaped_path[BRUCE_STORAGE_PATH_MAX * 2 + 8];
    if (!image_loader__escape_arg(path, escaped_path, sizeof(escaped_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }
    return app_runner__run("image_viewer", escaped_path, in_background);
}

int image_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Open an image in the image viewer.");
    ap_add_required_arg(parser, "path", "Path to a JPEG, PNG, or GIF image");
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    char *path = ap_get_arg(parser, "path");
    ap_free(parser);
    return image_loader__run_path(path, NULL, true);
}
