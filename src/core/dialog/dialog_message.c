#include "dialog_message.h"

#include "dialog_gui_common.h"

#include <stdint.h>

#include "core_sdk/display.h"
#include "core_sdk/input.h"

/* Mirrors the active font's cell size -- every built-in font is monospace
 * 6x10 today. */
#define DIALOG__CHAR_W 6
#define DIALOG__CHAR_H 10
#define DIALOG__WINDOW_RADIUS 5
#define DIALOG__WINDOW_INSET 4
/* Text size dialog__gui_message()/dialog__gui_message_show() draw their
 * title bar and body message at -- bigger than the rest of the chrome since
 * a message dialog is nothing but this text, so it reads as the main
 * content rather than another status line. */
#define DIALOG__MESSAGE_TEXT_SIZE 2

static bruce_result_t dialog__gui_wait_for_any_key(void) {
    /* Discard whatever's still queued (e.g. the press that opened this
     * screen) so it can't be misread as the dismiss key before the user has
     * even seen the message. */
    (void)input__flush();
    for (;;) {
        bruce_input_event_t ev;
        bruce_result_t result = input__read(&ev, 100);
        if (result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
        if (result == BRUCE_OK && ev.action == BRUCE_INPUT_PRESS) { return BRUCE_OK; }
    }
}

/* Shared frame for dialog__gui_message()/dialog__gui_message_show(): a
 * bordered popup window, same shape as a windowed dialog__choice() (see
 * dialog_choice.c's render_window path), rather than the full-bleed screen
 * every other dialog__gui_* draws. Its title bar is filled with `kind`'s own
 * accent color (success/warning/error, or pri for BRUCE_DIALOG_INFO) instead
 * of the plain pri a windowed choice list uses, so the color itself signals
 * the message's severity at a glance. */
static bruce_result_t
dialog__gui_message_frame(bruce_dialog_kind_t kind, const char *title, const char *message) {
    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    (void)sec;
    (void)text_muted;
    uint16_t accent = kind == BRUCE_DIALOG_SUCCESS   ? success
                      : kind == BRUCE_DIALOG_WARNING ? warning
                      : kind == BRUCE_DIALOG_ERROR   ? error
                                                     : pri;

    bruce_result_t frame_result = display__begin_frame();
    if (frame_result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
    if (frame_result != BRUCE_OK) { return frame_result; }
    (void)display__fill_screen(bg);

    /* Same screen-edge padding dialog__choice() gives its own windowed
     * popup, so a message dialog reads as the same widget at the same size. */
    int left = 12;
    int top = 8;
    int viewport_w = display__width() - 2 * left;
    int viewport_h = display__height() - 2 * top;
    int content_left = left + DIALOG__WINDOW_INSET;
    int content_top = top + DIALOG__WINDOW_INSET;
    int content_w = viewport_w - 2 * DIALOG__WINDOW_INSET;

    display__fill_round_rect(left, top, viewport_w, viewport_h, DIALOG__WINDOW_RADIUS, surface);

    int title_h = title != NULL && title[0] != '\0' ? DIALOG__CHAR_H * DIALOG__MESSAGE_TEXT_SIZE + 4 : 0;
    if (title_h > 0) {
        /* Flush to the window's square top edge, not content_left/top's
         * inset - display__draw_round_rect() below redraws the rounded
         * outline on top of it, clipping these corners back round. */
        display__fill_rect(left, top, viewport_w, title_h, accent);
        display__set_text_color(text);
        display__set_text_size(DIALOG__MESSAGE_TEXT_SIZE);
        display__set_text_bg_color(accent);
        display__set_cursor(content_left, content_top);
        display__print(title);
    }
    display__draw_round_rect(left, top, viewport_w, viewport_h, DIALOG__WINDOW_RADIUS, border);

    int max_chars = content_w / (DIALOG__CHAR_W * DIALOG__MESSAGE_TEXT_SIZE);
    if (max_chars < 1) { max_chars = 1; }

    display__set_text_color(text);
    display__set_text_size(DIALOG__MESSAGE_TEXT_SIZE);
    display__set_text_bg_color(surface);
    display__set_cursor(content_left, content_top + title_h + 4);

    if (message != NULL) {
        const char *p = message;
        int line_len = 0;
        while (*p != '\0') {
            if (*p == '\n' || line_len >= max_chars) {
                display__println("");
                line_len = 0;
                if (*p == '\n') {
                    p++;
                    continue;
                }
            }
            char ch[2] = {*p, '\0'};
            display__print(ch);
            line_len++;
            p++;
        }
    }

    return display__present();
}

bruce_result_t dialog__gui_message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    bruce_result_t frame_result = dialog__gui_message_frame(kind, title, message);
    if (frame_result != BRUCE_OK) {
        return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
    }
    return dialog__gui_wait_for_any_key();
}

bruce_result_t dialog__gui_message_show(bruce_dialog_kind_t kind, const char *title, const char *message) {
    bruce_result_t frame_result = dialog__gui_message_frame(kind, title, message);
    return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
}
