#include "image_test.h"

#include <stdio.h>

#include "core/display/display.h"
#include "core/image/gif/gif.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/display.h"
#include "core_sdk/image.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

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
    bool resize_ok = image__bitmap_resize(&source, 1, 1, &resized) == BRUCE_OK && resized.width == 1 &&
                      resized.height == 1 && resized.pixels != NULL && resized.external;
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    /* image__bitmap_resize() backs its output with memory__external_malloc(),
     * whose pointer QEMU's swap backend does not guarantee mirrors written
     * content on direct dereference (see memory_test.c's matching
     * CONFIG_BRUCE_QEMU_TEST_MODE guard around its own mapped-pointer
     * memcmp()) -- only real hardware can check the actual pixel value. */
    resize_ok = resize_ok && resized.pixels[0] == BRUCE_COLOR_RED;
#endif
    if (!resize_ok) {
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

/* Exercises image_app_main()'s "--stdin-size N" support (image_loader_app.c)
 * end to end: launched directly -- no shell involved, the same way
 * selftest__run_shell_cat_interactive_case() (shell_test.c) launches "cat" --
 * with its stdio routed to a session this test controls. Launched this way
 * (BRUCE_LAUNCH_BACKGROUND, no GUI=1), a successful decode also exercises
 * the piped path's runtime__to_foreground() self-promotion (image_app_main()
 * would otherwise draw into a hidden, frame_noop display context -- see
 * display.c -- and this pixel check would see nothing): polls
 * display__test_read_pixel() for the fixture's known color rather than
 * asserting immediately, since the child has to actually get scheduled and
 * draw first. Like every image, once drawn it waits for a keypress that
 * this piped process could never receive, so it never returns on its own;
 * process__wait_status() timing out with BRUCE_ERR_TIMEOUT confirms that
 * (the same signal process_test.c's spin-helper cases use), and
 * process__kill() cleans it up. A malformed --stdin-size, or a payload
 * image__get_bitmap_from_memory() can't decode, both return promptly
 * instead -- the latter via dialog__message()'s non-GUI fallback,
 * dialog__term_message(), which prints and returns rather than blocking for
 * a tap this process could never deliver either. */
bool selftest__run_image_stdin_pipe_case(void) {
    static const uint8_t red_gif[] = {
        'G', 'I',  'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0xff, 0,    0, 0,    0,
        0,   0x2c, 0,   0,   0,   0,   1, 0, 1, 0, 0,    2, 2, 0x44, 0x01, 0, 0x3b,
    };
    static const uint8_t invalid[] = {'n', 'o', 't', 'i', 'm', 'g'};
    char arg[32];

    snprintf(arg, sizeof(arg), "--stdin-size %u", (unsigned)sizeof(red_gif));
    bool decode_ok = false;
    bool pixel_ok = false;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    /* Retried up to a few times: image_app_main() draws with center=true,
     * fit=true, and image__fit_size() (render.c) scales this 1x1 fixture up
     * to fill a whole screen dimension, so decoding it needs a real
     * screen-sized resize buffer from the external heap -- right after the
     * loader selftests immediately ahead of this one in the full suite
     * (elf/wasm/js, all heavy external-heap users themselves) that
     * allocation can transiently fail with BRUCE_ERR_NO_MEMORY under
     * leftover fragmentation, clearing up again a moment later (the two
     * sub-cases below, running mere moments after this one, reliably
     * succeed even when this one hits it). Same category of environment-
     * driven flakiness image/decode's own CONFIG_BRUCE_QEMU_TEST_MODE guard
     * (above) works around, just transient instead of permanent. */
    for (int attempt = 0; attempt < 5 && !(decode_ok && pixel_ok); ++attempt) {
        if (attempt > 0) (void)runtime__delay(100);
        pixel_ok = false;
        session = BRUCE_STDIO_SESSION_INVALID;
        if (stdio__session_create(&session) == BRUCE_OK && stdio__session_route_children(session) == BRUCE_OK) {
            int launched = app_runner__run("image", arg, BRUCE_LAUNCH_BACKGROUND);
            (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
            if (launched > 0) {
                (void)stdio__session_write_input(session, red_gif, sizeof(red_gif));
                /* Not (0, 0): the fit-scaled square is centered, so it
                 * doesn't necessarily reach the corner, but the screen's
                 * center point is inside it regardless of aspect ratio.
                 * display__width()/height() are this (hidden, non-
                 * foreground) selftest process's own viewport -- 0 here,
                 * not the physical screen -- so the physical size comes
                 * from the same Kconfig macros display.c itself builds the
                 * real framebuffer from. */
                bruce_display_color_t pixel = 0;
                int16_t center_x = (int16_t)(CONFIG_BRUCE_DISPLAY_WIDTH / 2);
                int16_t center_y = (int16_t)(CONFIG_BRUCE_DISPLAY_HEIGHT / 2);
                for (int i = 0; i < 25 && !pixel_ok; ++i) {
                    (void)runtime__delay(20);
                    pixel_ok = display__test_read_pixel(center_x, center_y, &pixel) == BRUCE_OK &&
                               pixel == BRUCE_COLOR_RED;
                }
                bruce_process_status_t status;
                bruce_result_t wait_result = process__wait_status((bruce_process_id_t)launched, 100, &status);
                decode_ok = wait_result == BRUCE_ERR_TIMEOUT;
                if (!decode_ok && !(wait_result == BRUCE_OK && status.exit_code == BRUCE_ERR_NO_MEMORY)) {
                    /* Anything other than transient exhaustion is a real
                     * failure -- stop retrying and let it be reported. */
                    (void)process__kill((bruce_process_id_t)launched);
                    break;
                }
                (void)process__kill((bruce_process_id_t)launched);
            }
        }
        (void)stdio__session_close(session);
    }

    snprintf(arg, sizeof(arg), "--stdin-size %u", (unsigned)sizeof(invalid));
    bool invalid_payload_ok = false;
    session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) == BRUCE_OK && stdio__session_route_children(session) == BRUCE_OK) {
        int launched = app_runner__run("image", arg, BRUCE_LAUNCH_BACKGROUND);
        (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
        if (launched > 0) {
            (void)stdio__session_write_input(session, invalid, sizeof(invalid));
            bruce_process_status_t status;
            invalid_payload_ok =
                process__wait_status((bruce_process_id_t)launched, 2000, &status) == BRUCE_OK &&
                status.reason == BRUCE_PROCESS_EXITED && status.exit_code != 0;
            if (!invalid_payload_ok) (void)process__kill((bruce_process_id_t)launched);
        }
    }
    (void)stdio__session_close(session);

    session = BRUCE_STDIO_SESSION_INVALID;
    bool bad_size_ok = false;
    if (stdio__session_create(&session) == BRUCE_OK && stdio__session_route_children(session) == BRUCE_OK) {
        int launched = app_runner__run("image", "--stdin-size notanumber", BRUCE_LAUNCH_BACKGROUND);
        (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
        if (launched > 0) {
            bruce_process_status_t status;
            bad_size_ok = process__wait_status((bruce_process_id_t)launched, 2000, &status) == BRUCE_OK &&
                          status.reason == BRUCE_PROCESS_EXITED && status.exit_code != 0;
            if (!bad_size_ok) (void)process__kill((bruce_process_id_t)launched);
        }
    }
    (void)stdio__session_close(session);

    bool ok = decode_ok && pixel_ok && invalid_payload_ok && bad_size_ok;
    if (!ok) {
        printf(
            "[selftest] image/stdin_pipe: decode_ok=%d pixel_ok=%d invalid_payload_ok=%d bad_size_ok=%d\n",
            decode_ok, pixel_ok, invalid_payload_ok, bad_size_ok
        );
    }
    printf("[selftest] image/stdin_pipe: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises image_app_main()'s redraw-on-regain-foreground fix
 * (image_loader_app.c): input__read() returning BRUCE_ERR_NOT_FOREGROUND
 * only tells the viewer it lost and regained the screen -- whatever painted
 * over it while backgrounded isn't restored automatically (see process.h's
 * bruce_process_snapshot_t.blocked_on_wait doc comment), so
 * image_viewer__resume_after_handoff() returning true has to be followed by
 * an actual redraw (image_viewer__present_bitmap()), not a bare retry of the
 * input wait.
 *
 * Launches "image" directly on a small red fixture (a GIF, saved with a
 * ".png" extension so image_viewer__is_gif()'s extension check keeps it on
 * the static-image/retained-bitmap path this fix touches -- image decodes by
 * sniffing content, not by extension, so this still decodes fine), confirms
 * the initial draw, then plays the "another process steals the screen" role
 * itself: self-promotes via runtime__to_foreground() and paints the screen
 * blue (process__foreground_push_locked()'s recompute demotes "image" to
 * background as a side effect, the same as a real overlay or alt-tab would),
 * then calls process__foreground() -- unlike runtime__to_foreground(), not
 * self-only, so this driver can push "image"'s pid back to the front the
 * same way the launcher/alt-tab would -- and checks the screen goes back to
 * red. Without the fix it would stay stuck on blue. */
bool selftest__run_image_redraw_on_regain_case(void) {
    static const uint8_t red_gif[] = {
        'G', 'I',  'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0xff, 0,    0, 0,    0,
        0,   0x2c, 0,   0,   0,   0,   1, 0, 1, 0, 0,    2, 2, 0x44, 0x01, 0, 0x3b,
    };
    static const char *fixture_path = "/apps/image_selftest_redraw.png";
    int16_t center_x = (int16_t)(CONFIG_BRUCE_DISPLAY_WIDTH / 2);
    int16_t center_y = (int16_t)(CONFIG_BRUCE_DISPLAY_HEIGHT / 2);

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        fixture_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    size_t written = 0;
    if (result == BRUCE_OK) result = storage__write(file, red_gif, sizeof(red_gif), &written);
    if (result == BRUCE_OK && written != sizeof(red_gif)) result = BRUCE_ERR_IO;
    if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
    if (result != BRUCE_OK) {
        printf("[selftest] image/redraw_on_regain: FAIL, fixture write\n");
        return false;
    }

    bool ok = false;
    int launched = app_runner__run("image", fixture_path, BRUCE_LAUNCH_FOREGROUND);
    if (launched > 0) {
        bruce_display_color_t pixel = 0;
        bool drew_red = false;
        for (int i = 0; i < 25 && !drew_red; ++i) {
            (void)runtime__delay(20);
            drew_red = display__test_read_pixel(center_x, center_y, &pixel) == BRUCE_OK && pixel == BRUCE_COLOR_RED;
        }

        bool stole_screen = drew_red && runtime__to_foreground() == BRUCE_OK && display__begin_frame() == BRUCE_OK &&
                             display__fill_screen(BRUCE_COLOR_BLUE) == BRUCE_OK && display__present() == BRUCE_OK;
        /* Give "image"'s own task a chance to actually get scheduled and
         * notice the handoff while backgrounded: it only learns it lost the
         * foreground the next time it calls input__read() (image_app_main()
         * loops on that with a 100ms timeout, event_loop.c), which checks
         * current state at call time rather than an edge-triggered
         * notification of the transition. Handing the foreground back
         * before that first call lands would mean it never observes having
         * been backgrounded at all -- resume_after_handoff() and the redraw
         * it triggers would never run, and this check would pass for the
         * wrong reason (the fixture just never got overwritten in the first
         * place, not because the fix redrew it). */
        if (stole_screen) (void)runtime__delay(200);
        bool regained = stole_screen && process__foreground((bruce_process_id_t)launched) == BRUCE_OK;

        bool redrew_red = false;
        for (int i = 0; regained && i < 25 && !redrew_red; ++i) {
            (void)runtime__delay(20);
            redrew_red =
                display__test_read_pixel(center_x, center_y, &pixel) == BRUCE_OK && pixel == BRUCE_COLOR_RED;
        }

        ok = drew_red && stole_screen && regained && redrew_red;
        if (!ok) {
            printf(
                "[selftest] image/redraw_on_regain: drew_red=%d stole_screen=%d regained=%d redrew_red=%d\n",
                drew_red, stole_screen, regained, redrew_red
            );
        }
        (void)process__kill((bruce_process_id_t)launched);
    } else {
        printf("[selftest] image/redraw_on_regain: FAIL, launch\n");
    }
    (void)storage__remove(fixture_path);
    printf("[selftest] image/redraw_on_regain: %s\n", ok ? "OK" : "failed");
    return ok;
}
