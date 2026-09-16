#include "image_loader_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"

/* Same cap as bnu_pager_app.c's "less" uses for its own "--stdin-size"
 * fixture (LESS_MAX_BYTES) -- plenty for anything this display can show, and
 * small enough that a runaway/adversarial byte count can't be used to exhaust
 * the external heap a piped `curl url | image` decodes into. */
#define IMAGE_STDIN_MAX_BYTES (512u * 1024u)

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

/* Mirrors bnu_pager_app.c's less__load_stdin(): reads exactly `size` bytes
 * (the shell already knows this count -- see shell_executor__pipe_write()'s
 * "--stdin-size N" convention -- because piping has no real EOF signal here)
 * into an owned memory__external_malloc() buffer, chunked through a small
 * stack buffer since stdio__read() has no direct-into-external-memory form. */
static bruce_result_t image_viewer__load_stdin(size_t size, const void **out_data, size_t *out_length) {
    const void *data = NULL;
    bruce_result_t result = BRUCE_OK;
    if (size > 0) {
        data = memory__external_malloc(size);
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    unsigned char chunk[256];
    while (result == BRUCE_OK && offset < size) {
        size_t want = size - offset > sizeof(chunk) ? sizeof(chunk) : size - offset;
        size_t read_size = 0;
        result = stdio__read(chunk, want, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) result = memory__external_memcpy(data, offset, chunk, read_size);
        offset += read_size;
    }
    if (result != BRUCE_OK) {
        if (data != NULL) (void)memory__external_free(data);
        return result;
    }
    *out_data = data;
    *out_length = offset;
    return BRUCE_OK;
}

/* Shared by the first draw and by the redraw-on-regain-foreground path
 * below: whatever painted over the screen while this process was
 * backgrounded (a system-menu overlay, another foreground app) isn't
 * restored automatically on switching back (see process.h's
 * bruce_process_snapshot_t.blocked_on_wait doc comment) -- each foregrounded
 * process must redraw itself. */
static bruce_result_t
image_viewer__present_bitmap(const image_bitmap_t *bitmap, const bruce_image_draw_options_t *options) {
    bruce_result_t result = display__begin_frame();
    if (result == BRUCE_OK) result = display__fill_screen(options->background);
    if (result == BRUCE_OK) result = image__draw_bitmap(bitmap, options);
    if (result == BRUCE_OK) result = display__present();
    return result;
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
    ap_set_helptext(parser, "Display an image, or piped stdin, until an input event is received.");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "path", "Path to a JPEG, PNG, or GIF image");
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
    const char *stdin_size_arg = ap_found(parser, "stdin-size") ? ap_get_str_value(parser, "stdin-size") : NULL;
    char *end = NULL;
    unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
    bool stdin_requested = stdin_size_arg != NULL;
    bool from_stdin = stdin_requested && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0' &&
                      parsed_stdin_size <= IMAGE_STDIN_MAX_BYTES;
    ap_free(parser);

    if (stdin_requested && !from_stdin) {
        stdio__printf("image: invalid --stdin-size\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!from_stdin && path == NULL) {
        stdio__printf("image: missing path\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    /* A piped payload is decoded by content (image__get_bitmap_from_memory()
     * sniffs the signature), not by a filename extension it doesn't have. */
    if (!from_stdin && !image__is_supported_path(path)) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bruce_image_draw_options_t options = {
        .center = true,
        .fit = true,
        .background = BRUCE_COLOR_BLACK,
    };
    /* A pipe destination always launches backgrounded, with no per-stage
     * GUI=1 support (see shell_executor__pipe_write()'s doc comment) -- a
     * background process's display context stays hidden (display.c), so
     * every draw call below would silently no-op without this, and
     * image__fit_size() (render.c), which the decode step below calls to
     * size the bitmap to the screen, reads this same process's own
     * display__width()/height() -- 0 until promoted -- so promoting has to
     * happen before decoding too, not just before drawing. Same
     * self-promotion wifi_app.c's wifi_app_on() uses to reach the
     * foreground from a process that didn't start there. A path-based
     * launch needs no such push: interactively typing "image foo.png"
     * already starts in the foreground. */
    bruce_result_t result = from_stdin ? runtime__to_foreground() : BRUCE_OK;
    if (result == BRUCE_OK) result = display__begin_frame();
    if (result == BRUCE_OK) result = display__fill_screen(options.background);
    if (result == BRUCE_OK) result = display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    if (result == BRUCE_OK) result = display__set_text_color(BRUCE_COLOR_WHITE);
    if (result == BRUCE_OK) result = display__set_text_size(2);
    if (result == BRUCE_OK) {
        result = display__draw_centre_string("Loading...", display__width() / 2, (display__height() - 8) / 2);
    }
    if (result == BRUCE_OK) result = display__present();
    /* image__get_bitmap_from_memory() can decode a GIF too (its first frame
     * only, no animation) -- there's no memory-based equivalent of the
     * animated image__gif_open()/_draw() loop, so a piped GIF always falls
     * into the static branch below rather than image_viewer__draw_gif(). */
    bool is_gif = !from_stdin && image_viewer__is_gif(path);
    /* Retained (not released right after this first draw, unlike the single-
     * shot image__draw_path() the non-piped case used to call directly) so
     * the redraw-on-regain-foreground path below can redraw it again without
     * re-decoding. */
    image_bitmap_t bitmap = {0};
    bool have_bitmap = false;
    if (is_gif) {
        if (result == BRUCE_OK) result = image_viewer__draw_gif(path, &options);
    } else {
        if (result == BRUCE_OK) {
            if (from_stdin) {
                const void *stdin_data = NULL;
                size_t stdin_length = 0;
                result = image_viewer__load_stdin((size_t)parsed_stdin_size, &stdin_data, &stdin_length);
                if (result == BRUCE_OK) {
                    result = image__get_bitmap_from_memory(stdin_data, stdin_length, &options, &bitmap);
                    (void)memory__external_free(stdin_data);
                }
            } else {
                result = image__get_bitmap_from_file(path, &options, &bitmap);
            }
        }
        have_bitmap = result == BRUCE_OK;
        if (result == BRUCE_OK) result = image_viewer__present_bitmap(&bitmap, &options);
    }
    if (result != BRUCE_OK) {
        if (have_bitmap) image__bitmap_release(&bitmap);
        char message[64];
        snprintf(message, sizeof(message), "Could not load image: %s", result__to_string(result));
        /* dialog__message() would wait for a keypress this piped process
         * could never deliver: it's already promoted to the foreground
         * above (needed for image__fit_size()'s viewport reads during
         * decode), so dialog__current_process_wants_gui() picks the GUI
         * dialog form here rather than the non-blocking term fallback a
         * non-piped caller's decode failure would have gotten. Same
         * "draw once, don't wait to be dismissed" dialog__message_show()
         * a slow blocking call with no natural point to poll from uses
         * (core_sdk/dialog.h) -- nothing needs to dismiss this either, since
         * the process exits right after. */
        if (from_stdin) {
            (void)dialog__message_show(BRUCE_DIALOG_ERROR, "Image", message);
        } else {
            (void)dialog__message(BRUCE_DIALOG_ERROR, "Image", message);
        }
        return result;
    }

    if (!is_gif) {
        (void)input__flush();
        for (;;) {
            bruce_input_event_t event;
            bruce_result_t input_result = input__read(&event, 100);
            if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
                if (image_viewer__resume_after_handoff()) {
                    (void)image_viewer__present_bitmap(&bitmap, &options);
                    continue;
                }
                break;
            }
            if (input_result == BRUCE_OK && event.action == BRUCE_INPUT_PRESS) break;
        }
        image__bitmap_release(&bitmap);
    }
    return result;
}
