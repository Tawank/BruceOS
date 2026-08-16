#include "image_loader_app.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"

static bool image_viewer__is_gif(const char *path) {
    const char *extension = path != NULL ? strrchr(path, '.') : NULL;
    return extension != NULL && strcasecmp(extension, ".gif") == 0;
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
        if (input_result == BRUCE_ERR_NOT_FOREGROUND ||
            (input_result == BRUCE_OK && event.action == BRUCE_INPUT_PRESS))
            break;
        result = image__gif_increment(gif, NULL);
    }
    image__gif_close(gif);
    return result;
}

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
    if (!image__is_supported_path(path)) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bruce_image_draw_options_t options = {
        .center = true,
        .fit = true,
        .background = BRUCE_COLOR_BLACK,
    };
    bruce_result_t result;
    if (image_viewer__is_gif(path)) {
        result = image_viewer__draw_gif(path, &options);
    } else {
        result = display__begin_frame();
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
        snprintf(message, sizeof(message), "Image error (%d)", (int)result);
        (void)display__println(message);
        (void)display__present();
    }

    if (!image_viewer__is_gif(path) || result != BRUCE_OK) {
        (void)input__flush();
        for (;;) {
            bruce_input_event_t event;
            bruce_result_t input_result = input__read(&event, 100);
            if (input_result == BRUCE_ERR_NOT_FOREGROUND) break;
            if (input_result == BRUCE_OK && event.action == BRUCE_INPUT_PRESS) break;
        }
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
    if (written + 1u > out_size) return false;
    out[written] = '\0';
    return true;
}

static int image_loader__open(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    (void)arg;
    if (path == NULL || !image__is_supported_path(path)) return BRUCE_ERR_INVALID_PATH;

    char escaped_path[BRUCE_STORAGE_PATH_MAX * 2 + 8];
    if (!image_loader__escape_arg(path, escaped_path, sizeof(escaped_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }

    bruce_environment_variable_t merged[BRUCE_ENVIRONMENT_MAX_VARIABLES];
    size_t merged_count = 0;
    for (size_t i = 0; i < environment_count && merged_count < BRUCE_ENVIRONMENT_MAX_VARIABLES - 1u; ++i) {
        merged[merged_count++] = environment[i];
    }
    merged[merged_count++] = (bruce_environment_variable_t){.name = "GUI", .value = "1"};
    return app_runner__run_with_environment("image_viewer", escaped_path, mode, merged, merged_count);
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
    bruce_process_snapshot_t snapshot;
    bruce_launch_mode_t mode = BRUCE_LAUNCH_FOREGROUND;
    if (process__snapshot(process__current_id(), &snapshot) == BRUCE_OK &&
        snapshot.state == BRUCE_PROCESS_BACKGROUND) {
        mode = BRUCE_LAUNCH_BACKGROUND;
    }
    return image_loader__open(path, NULL, mode, NULL, 0);
}
