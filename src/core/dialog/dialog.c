#include "dialog.h"

#include "core/config/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/icon.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/memory.h"
#include "core_sdk/partition_manager.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#include "core/process/process.h"
#include "core/storage/storage.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* -------------------------------------------------------------------------- */
/* Test provider hooks (see core/dialog/dialog.h)                             */
/* -------------------------------------------------------------------------- */

static dialog__test_choice_provider_t s_test_choice_provider;
static dialog__test_input_provider_t s_test_input_provider;
static dialog__test_pick_file_provider_t s_test_pick_file_provider;
static bool s_last_call_was_gui;

void dialog__test_set_choice_provider(dialog__test_choice_provider_t provider) {
    s_test_choice_provider = provider;
}

void dialog__test_set_input_provider(dialog__test_input_provider_t provider) {
    s_test_input_provider = provider;
}

void dialog__test_set_pick_file_provider(dialog__test_pick_file_provider_t provider) {
    s_test_pick_file_provider = provider;
}

bool dialog__test_last_call_was_gui(void) { return s_last_call_was_gui; }

/* -------------------------------------------------------------------------- */
/* Renderer selection                                                         */
/* -------------------------------------------------------------------------- */

static bool dialog__current_process_wants_gui(void) {
    bool gui_requested = false;
    (void)process_registry__current_context(NULL, NULL, 0, &gui_requested);
    return gui_requested;
}

static const char *dialog__kind_label(bruce_dialog_kind_t kind) {
    switch (kind) {
        case BRUCE_DIALOG_INFO: return "info";
        case BRUCE_DIALOG_SUCCESS: return "success";
        case BRUCE_DIALOG_WARNING: return "warning";
        case BRUCE_DIALOG_ERROR: return "error";
        default: return "info";
    }
}

/* -------------------------------------------------------------------------- */
/* Terminal renderers                                                         */
/* -------------------------------------------------------------------------- */

static bruce_result_t dialog__term_message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    stdio__printf(
        "[dialog:%s:%s] %s%s%s\n",
        "term",
        dialog__kind_label(kind),
        title != NULL ? title : "",
        title != NULL && message != NULL ? ": " : "",
        message != NULL ? message : ""
    );
    return BRUCE_OK;
}

static int dialog__term_read_line(char *buffer, size_t buffer_size, bool mask_input) {
    return stdio__read_line(buffer, buffer_size, mask_input);
}

static bruce_result_t dialog__term_choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    if (title != NULL) { stdio__printf("%s\n", title); }
    if (message != NULL) { stdio__printf("%s\n", message); }
    for (size_t i = 0; i < choice_count; ++i) {
        stdio__printf("%u. %s", (unsigned int)(i + 1), choices[i].label != NULL ? choices[i].label : "");
        if (choices[i].right_text != NULL && choices[i].right_text[0] != '\0') {
            stdio__printf("  %s", choices[i].right_text);
        }
        stdio__printf("\n");
    }
    stdio__printf("pick: ");

    char line[16];
    if (dialog__term_read_line(line, sizeof(line), false) < 0) { return BRUCE_ERR_CANCELLED; }
    char *end = NULL;
    long picked = strtol(line, &end, 10);
    if (end == line || picked < 1 || (size_t)picked > choice_count) { return BRUCE_ERR_INVALID_ARGUMENT; }
    *out_selected = (size_t)(picked - 1);
    return BRUCE_OK;
}

static bruce_result_t dialog__term_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size, bool (*validate)(const char *text, size_t len)
) {
    if (title != NULL) { stdio__printf("%s\n", title); }
    if (prompt != NULL) { stdio__printf("%s", prompt); }
    if (initial_text != NULL && initial_text[0] != '\0') { stdio__printf(" [%s]", initial_text); }
    stdio__printf(": ");

    char tmp[256];
    size_t tmp_size = buffer_size < sizeof(tmp) ? buffer_size : sizeof(tmp);
    if (tmp_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    int len = dialog__term_read_line(tmp, tmp_size, mask_input);
    if (len < 0) { return BRUCE_ERR_CANCELLED; }

    if (tmp[0] == '\0' && initial_text != NULL) {
        snprintf(buffer, buffer_size, "%s", initial_text);
    } else {
        snprintf(buffer, buffer_size, "%s", tmp);
    }

    if (validate != NULL && !validate(buffer, strlen(buffer))) {
        stdio__printf("Invalid input.\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    return BRUCE_OK;
}

static bool dialog__validate_hex(const char *text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (!isxdigit((unsigned char)c)) { return false; }
    }
    return true;
}

static bool dialog__validate_number(const char *text, size_t len) {
    bool has_dot = false;
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '-') {
            if (i != 0) { return false; }
        } else if (c == '.') {
            if (has_dot) { return false; }
            has_dot = true;
        } else if (!isdigit((unsigned char)c)) {
            return false;
        }
    }
    return true;
}

static bruce_result_t dialog__term_pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size
) {
    const char *path = initial_path != NULL && initial_path[0] != '\0' ? initial_path : "/";
    stdio__printf("Enter file path");
    if (extension_filter != NULL) { stdio__printf(" (%s)", extension_filter); }
    stdio__printf(" [%s]: ", path);

    char line[BRUCE_STORAGE_PATH_MAX];
    int len = dialog__term_read_line(line, sizeof(line), false);
    if (len < 0) { return BRUCE_ERR_CANCELLED; }

    if (line[0] == '\0') {
        snprintf(out_path, out_path_size, "%s", path);
    } else {
        snprintf(out_path, out_path_size, "%s", line);
    }
    return BRUCE_OK;
}

/* -------------------------------------------------------------------------- */
/* GUI common helpers                                                         */
/* -------------------------------------------------------------------------- */

/* Mirrors the active font's cell size (display__get_font_metrics()) as
 * compile-time constants -- every built-in font is monospace 6x10 today. */
#define DIALOG__CHAR_W 6
#define DIALOG__CHAR_H 10
#define DIALOG__TEXT_SIZE 1
#define DIALOG__MARGIN 2
/* Width threshold above which choice-list rows step up to a larger font,
 * matching bruce_launcher__submenu_font_size() in bruce_launcher_app.c so a
 * menu screen (e.g. Config, App Permissions) reads at the same size whether
 * it's rendered by the launcher's own submenu list or by dialog__choice().
 * Title bars/footers/messages stay at DIALOG__TEXT_SIZE, same as the
 * launcher's small header/status text. */
#define DIALOG__WIDE_DISPLAY_MIN_WIDTH 200
#define DIALOG__LIST_TEXT_SIZE_WIDE 2
#define DIALOG__WINDOW_RADIUS 5
#define DIALOG__WINDOW_INSET 4
/* Vertical breathing room dialog__gui_pick_file() requests (via
 * render_params->list_gap) between its title bar and the first list row -
 * without it, a selected first row's fill paints flush against the title
 * bar above it and the two visually merge into one block. Opt-in per
 * render_params, not a default in dialog__gui_choice(), so plain
 * dialog__choice()/_ex() callers keep their existing flush layout. */
#define DIALOG__LIST_GAP 3

static int dialog__default_list_text_size(void) {
    return display__width() >= DIALOG__WIDE_DISPLAY_MIN_WIDTH ? DIALOG__LIST_TEXT_SIZE_WIDE
                                                              : DIALOG__TEXT_SIZE;
}

/* pri/sec/bg are the accent, secondary-accent, and full-screen-canvas
 * colors; surface/border are for a raised bordered window on top of that
 * canvas (see the render_window path in dialog__gui_choice()); text is the
 * readable color drawn over pri/sec-filled chrome (title/footer bars) and
 * over bg/surface bodies alike - a light theme can set it dark and every
 * one of those spots stays legible without each caller guessing;
 * success/warning/error are dialog__gui_message()'s BRUCE_DIALOG_SUCCESS /
 * _WARNING / _ERROR title-bar accent. */
static void dialog__get_colors(
    uint16_t *pri, uint16_t *sec, uint16_t *bg, uint16_t *surface, uint16_t *text, uint16_t *text_muted,
    uint16_t *border, uint16_t *success, uint16_t *warning, uint16_t *error
) {
    config__get_colors_internal(pri, sec, bg, surface, text, text_muted, border, success, warning, error);
}

static void dialog__gui_clear(void) {
    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    (void)display__fill_screen(bg);
}

/* `accent` fills the bar (normally the primary color, but
 * dialog__gui_message() substitutes success/warning/error for its kind);
 * `text` is the color its label is drawn in. */
static void dialog__gui_title_bar_accent(const char *title, uint16_t accent, uint16_t text) {
    int w = display__width();
    display__fill_rect(0, 0, w, DIALOG__CHAR_H + 4, accent);
    display__set_text_color(text);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(accent);
    display__set_cursor(DIALOG__MARGIN, DIALOG__MARGIN);
    display__print(title != NULL ? title : "");
}

static void dialog__gui_title_bar(const char *title) {
    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    dialog__gui_title_bar_accent(title, pri, text);
}

static void dialog__gui_footer(const char *hint) {
    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);

    int w = display__width();
    int h = display__height();
    display__fill_rect(0, h - DIALOG__CHAR_H - 4, w, DIALOG__CHAR_H + 4, sec);
    display__set_text_color(text);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(sec);
    display__set_cursor(DIALOG__MARGIN, h - DIALOG__CHAR_H - 2);
    display__print(hint != NULL ? hint : "");
}

/* The display text API wraps at the viewport edge. Limit a row label before
 * drawing it so optional icons and right-aligned text never overlap it. */
static void dialog__gui_draw_row_label(const char *label, int x, int y, int max_width, int text_size) {
    if (label == NULL || max_width <= 0) { return; }
    int max_chars = max_width / (DIALOG__CHAR_W * text_size);
    if (max_chars <= 0) { return; }

    size_t label_len = strlen(label);
    size_t columns = 0;
    for (size_t i = 0; i < label_len;) {
        unsigned char c = (unsigned char)label[i];
        size_t bytes = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2);
        if (i + bytes > label_len) bytes = 1;
        i += bytes;
        columns++;
    }
    size_t draw_len = label_len;
    if (columns > (size_t)max_chars) {
        size_t column = 0;
        draw_len = 0;
        while (draw_len < label_len && column < (size_t)max_chars) {
            unsigned char c = (unsigned char)label[draw_len];
            size_t bytes = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2);
            if (draw_len + bytes > label_len) bytes = 1;
            draw_len += bytes;
            column++;
        }
    }
    if (columns <= (size_t)max_chars) {
        display__set_cursor(x, y);
        display__print(label);
        return;
    }

    if (max_chars <= 3) {
        display__set_cursor(x, y);
        for (int i = 0; i < max_chars; ++i) { display__print("."); }
        return;
    }

    char truncated[128];
    size_t wanted = (size_t)(max_chars - 3);
    draw_len = 0;
    size_t column = 0;
    while (draw_len < label_len && column < wanted) {
        unsigned char c = (unsigned char)label[draw_len];
        size_t bytes = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2);
        if (draw_len + bytes > label_len) bytes = 1;
        if (draw_len + bytes >= sizeof(truncated) - 4) break;
        draw_len += bytes;
        column++;
    }
    memcpy(truncated, label, draw_len);
    memcpy(truncated + draw_len, "...", 4);
    display__set_cursor(x, y);
    display__print(truncated);
}

/* -------------------------------------------------------------------------- */
/* Window renderer registry                                           */
/* -------------------------------------------------------------------------- */

static bruce_dialog_window_renderer_t s_window_renderer;
static void *s_window_renderer_context;
static bool s_window_renderer_set;

void dialog__set_window_renderer(const bruce_dialog_window_renderer_t *renderer, void *context) {
    if (renderer == NULL) {
        s_window_renderer_set = false;
        s_window_renderer_context = NULL;
        return;
    }
    s_window_renderer = *renderer;
    s_window_renderer_context = context;
    s_window_renderer_set = true;
}

bruce_dialog_render_params_t dialog__default_render_params(int text_size) {
    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    bruce_dialog_render_params_t params = {0};
    params.render_borders = true;
    params.text_size = text_size > 0 ? text_size : dialog__default_list_text_size();
    params.background_color = bg;
    /* pri, not `text`: this is also the color list rows (folders/files in
     * the picker) draw their label in, over background_color, so it should
     * stay the theme accent. The title bar's own text needs a color that
     * survives being filled with pri underneath it - dialog__gui_choice()
     * picks that separately rather than borrowing this field. */
    params.text_color = pri;
    return params;
}

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

static bruce_result_t dialog__gui_message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    uint16_t accent = kind == BRUCE_DIALOG_SUCCESS ? success
                      : kind == BRUCE_DIALOG_WARNING ? warning
                      : kind == BRUCE_DIALOG_ERROR   ? error
                                                      : pri;

    bruce_result_t frame_result = display__begin_frame();
    if (frame_result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
    if (frame_result != BRUCE_OK) { return frame_result; }
    (void)display__fill_screen(bg);
    dialog__gui_title_bar_accent(title, accent, text);

    int w = display__width();
    int max_chars = (w - 2 * DIALOG__MARGIN) / DIALOG__CHAR_W;
    if (max_chars < 1) { max_chars = 1; }

    display__set_text_color(text);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(bg);
    display__set_cursor(DIALOG__MARGIN, DIALOG__CHAR_H + 8);

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

    dialog__gui_footer("OK: select");
    frame_result = display__present();
    if (frame_result != BRUCE_OK) {
        return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
    }
    return dialog__gui_wait_for_any_key();
}

static bruce_result_t dialog__gui_choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected, const bruce_dialog_render_params_t *render_params
) {
    bool render_launcher = render_params != NULL && render_params->render_launcher && s_window_renderer_set;
    int selected = out_selected != NULL && *out_selected < choice_count ? (int)*out_selected : 0;
    int first_visible = 0;
    int w = display__width();
    int h = display__height();
    int left = render_launcher ? s_window_renderer.padding_left
                               : (render_params != NULL ? render_params->padding_left : 0);
    int top = render_launcher ? s_window_renderer.padding_top
                              : (render_params != NULL ? render_params->padding_top : 0);
    int right = w - (render_launcher ? s_window_renderer.padding_right
                                     : (render_params != NULL ? render_params->padding_right : 0));
    int bottom = h - (render_launcher ? s_window_renderer.padding_bottom
                                      : (render_params != NULL ? render_params->padding_bottom : 0));
    bool render_borders = !render_launcher && (render_params == NULL || render_params->render_borders);
    bool render_window = render_borders && render_params != NULL &&
                         (render_params->padding_top > 0 || render_params->padding_right > 0 ||
                          render_params->padding_bottom > 0 || render_params->padding_left > 0);
    int viewport_w = right - left;
    int viewport_h = bottom - top;
    int text_size = render_params != NULL && render_params->text_size > 0 ? render_params->text_size
                    : render_launcher && s_window_renderer.text_size > 0  ? s_window_renderer.text_size
                    : render_launcher                                     ? 1
                                                                          : dialog__default_list_text_size();

    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    (void)text_muted;
    (void)success;
    (void)warning;
    (void)error;
    bruce_display_color_t background_color =
        render_launcher ? bg : (render_params != NULL ? render_params->background_color : bg);
    bruce_display_color_t text_color =
        render_launcher ? pri : (render_params != NULL ? render_params->text_color : pri);
    uint32_t refresh_interval_ms = render_launcher         ? s_window_renderer.status_refresh_interval_ms
                                   : render_params != NULL ? render_params->refresh_interval_ms
                                                           : 0u;
    int content_left = left + (render_window ? DIALOG__WINDOW_INSET : 0);
    int content_top = top + (render_window ? DIALOG__WINDOW_INSET : 0);
    int content_w = viewport_w - (render_window ? 2 * DIALOG__WINDOW_INSET : 0);
    int content_h = viewport_h - (render_window ? 2 * DIALOG__WINDOW_INSET : 0);
    if (content_w <= 0 || content_h <= 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    int row_h = DIALOG__CHAR_H * text_size + 2;
    int title_h = render_window ? (title != NULL && title[0] != '\0' ? DIALOG__CHAR_H + 4 : 0)
                  : render_borders || (title != NULL && title[0] != '\0') ? DIALOG__CHAR_H + 4
                                                                          : 0;
    int footer_h = render_window ? 0 : (render_borders ? DIALOG__CHAR_H + 4 : 0);
    int message_h = (message != NULL && message[0] != '\0') ? (DIALOG__CHAR_H + 2) : 0;
    int list_gap = (title_h > 0 || message_h > 0) && render_params != NULL ? render_params->list_gap : 0;
    int usable_h = content_h - title_h - message_h - footer_h - list_gap;
    if (usable_h < row_h) { return BRUCE_ERR_INVALID_ARGUMENT; }
    int items_per_page = usable_h / row_h;
    bool redraw = true;
    bool launcher_border_drawn = false;
    uint64_t rendered_at = 0;
    bool wants_periodic_refresh = render_launcher
                                      ? s_window_renderer.draw_status != NULL
                                      : render_params != NULL && render_params->render_callback != NULL;

    /* Discard whatever's still queued (typically the press that navigated
     * into this screen) so it can't be replayed as an immediate selection on
     * the freshly drawn list before the user has seen it - e.g. a permission
     * prompt silently auto-confirming "Allow" on the stale press that
     * launched the requesting app. */
    (void)input__flush();
    for (;;) {
        uint64_t now = runtime__now();
        if (wants_periodic_refresh && refresh_interval_ms > 0 && now - rendered_at >= refresh_interval_ms) {
            redraw = true;
        }

        if (redraw) {
            bruce_result_t frame_result = display__begin_frame();
            if (frame_result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
            if (frame_result != BRUCE_OK) { return frame_result; }
            if (selected < first_visible) {
                first_visible = selected;
            } else if (selected >= first_visible + items_per_page) {
                first_visible = selected - items_per_page + 1;
            }

            if (render_launcher && !launcher_border_drawn) {
                if (s_window_renderer.draw_border != NULL) {
                    s_window_renderer.draw_border(s_window_renderer_context);
                }
                launcher_border_drawn = true;
            }

            if (render_window) {
                /* A bordered window floats a raised panel over whatever the
                 * launcher already drew as background, so it uses the
                 * surface/border pair rather than background_color/pri -
                 * see dialog__get_colors(). */
                display__fill_round_rect(left, top, viewport_w, viewport_h, DIALOG__WINDOW_RADIUS, surface);
                display__draw_round_rect(left, top, viewport_w, viewport_h, DIALOG__WINDOW_RADIUS, border);
            } else {
                display__fill_rect(left, top, viewport_w, viewport_h, background_color);
            }
            /* The color actually painted behind body text: the surface fill
             * in a bordered window, background_color everywhere else. */
            bruce_display_color_t content_fill_color = render_window ? surface : background_color;

            if (title_h > 0) {
                bool title_on_pri_fill = render_borders && !render_window;
                if (title_on_pri_fill) {
                    display__fill_rect(left, top, viewport_w, title_h, pri);
                }
                /* Full-bleed layout fills the bar itself with pri, so the
                 * label needs the readable-over-accent `text` color there
                 * instead of text_color (which stays pri, for list rows -
                 * see dialog__default_render_params()); a windowed dialog's
                 * bar sits on content_fill_color (surface) instead, where
                 * text_color already reads fine. */
                display__set_text_color(title_on_pri_fill ? text : text_color);
                display__set_text_size(DIALOG__TEXT_SIZE);
                display__set_text_bg_color(title_on_pri_fill ? pri : content_fill_color);
                display__set_cursor(content_left + DIALOG__MARGIN, content_top + DIALOG__MARGIN);
                display__print(title != NULL ? title : "");
            }

            if (message_h > 0) {
                display__set_text_color(text_color);
                display__set_text_size(DIALOG__TEXT_SIZE);
                display__set_text_bg_color(content_fill_color);
                display__set_cursor(content_left + DIALOG__MARGIN, content_top + title_h + 1);
                display__print(message);
            }

            int list_y = content_top + title_h + message_h + list_gap;
            int last_visible = first_visible + items_per_page - 1;
            if ((size_t)last_visible >= choice_count) { last_visible = (int)choice_count - 1; }

            for (int i = first_visible; i <= last_visible; ++i) {
                int y = list_y + (i - first_visible) * row_h;
                if (i == selected) {
                    display__fill_rect(content_left, y, content_w, row_h, text_color);
                    display__set_text_color(background_color);
                } else {
                    display__set_text_color(text_color);
                }
                display__set_text_size(text_size);
                display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
                int label_left = content_left + DIALOG__MARGIN;
                int label_right = content_left + content_w - DIALOG__MARGIN;
                const bruce_icon_t *icon = icon__get(choices[i].icon_name);
                if (icon != NULL) {
                    int icon_size = row_h - 2;
                    display__draw_bitmap_scaled(
                        label_left, y + 1, icon->bits, icon->width, icon->height, icon_size, icon_size,
                        i == selected ? background_color : text_color
                    );
                    label_left += icon_size + DIALOG__MARGIN;
                }
                if (choices[i].right_text != NULL && choices[i].right_text[0] != '\0') {
                    display__draw_right_string(choices[i].right_text, label_right, y + 1);
                    size_t right_columns = 0;
                    for (const unsigned char *p = (const unsigned char *)choices[i].right_text; *p != '\0';) {
                        size_t bytes = *p < 0x80 ? 1 : (*p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : 2);
                        p += bytes;
                        right_columns++;
                    }
                    label_right -= (int)right_columns * DIALOG__CHAR_W * text_size + DIALOG__MARGIN;
                }
                dialog__gui_draw_row_label(
                    choices[i].label, label_left, y + 1, label_right - label_left, text_size
                );
            }

            if (render_borders && !render_window) {
                display__fill_rect(left, bottom - footer_h, viewport_w, footer_h, sec);
            }
            if (render_launcher) {
                if (s_window_renderer.draw_status != NULL) {
                    s_window_renderer.draw_status(s_window_renderer_context);
                }
            } else if (render_params != NULL && render_params->render_callback != NULL) {
                render_params->render_callback(render_params->render_callback_context);
            }
            frame_result = display__present();
            if (frame_result != BRUCE_OK) {
                return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
            }
            rendered_at = runtime__now();
            redraw = false;
        }

        bruce_input_event_t ev;
        bruce_result_t input_result = input__read(&ev, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
        if (input_result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) { continue; }

        int previous_selected = selected;
        switch (ev.code) {
            case BRUCE_INPUT_CODE_UP:
            case BRUCE_INPUT_CODE_PREV:
                if (selected > 0) { selected--; }
                break;
            case BRUCE_INPUT_CODE_DOWN:
            case BRUCE_INPUT_CODE_NEXT:
                if ((size_t)selected + 1 < choice_count) { selected++; }
                break;
            case BRUCE_INPUT_CODE_LEFT:
                if (selected > 0) {
                    selected -= items_per_page;
                    if (selected < 0) { selected = 0; }
                }
                break;
            case BRUCE_INPUT_CODE_RIGHT:
                if (selected + items_per_page < (int)choice_count) {
                    selected += items_per_page;
                } else {
                    selected = (int)choice_count - 1;
                }
                break;
            case BRUCE_INPUT_CODE_SELECT:
            case '\r': *out_selected = (size_t)selected; return BRUCE_OK;
            case BRUCE_INPUT_CODE_BACK: return BRUCE_ERR_CANCELLED;
            default: break;
        }
        redraw = selected != previous_selected;
    }
}

/* -------------------------------------------------------------------------- */
/* GUI on-screen keyboard                                                     */
/* -------------------------------------------------------------------------- */

#define DIALOG__KEY_OK 1
#define DIALOG__KEY_CANCEL 2
#define DIALOG__KEY_DELETE 3
#define DIALOG__KEY_SPACE 4
#define DIALOG__KEY_CAPS 5

typedef enum {
    BRUCE_DIALOG_INPUT_TEXT,
    BRUCE_DIALOG_INPUT_HEX,
    BRUCE_DIALOG_INPUT_NUMBER,
} bruce_dialog_input_kind_t;

typedef struct {
    const char *label;
    char code;
    int special;
} dialog__key_t;

#define DIALOG__TEXT_COLS 10
#define DIALOG__TEXT_ROWS 5

static const dialog__key_t s_text_keys[DIALOG__TEXT_ROWS][DIALOG__TEXT_COLS] = {
    {
     {"1", '1', 0},
     {"2", '2', 0},
     {"3", '3', 0},
     {"4", '4', 0},
     {"5", '5', 0},
     {"6", '6', 0},
     {"7", '7', 0},
     {"8", '8', 0},
     {"9", '9', 0},
     {"0", '0', 0},
     },
    {
     {"q", 'q', 0},
     {"w", 'w', 0},
     {"e", 'e', 0},
     {"r", 'r', 0},
     {"t", 't', 0},
     {"y", 'y', 0},
     {"u", 'u', 0},
     {"i", 'i', 0},
     {"o", 'o', 0},
     {"p", 'p', 0},
     },
    {
     {"a", 'a', 0},
     {"s", 's', 0},
     {"d", 'd', 0},
     {"f", 'f', 0},
     {"g", 'g', 0},
     {"h", 'h', 0},
     {"j", 'j', 0},
     {"k", 'k', 0},
     {"l", 'l', 0},
     {";", ';', 0},
     },
    {
     {"z", 'z', 0},
     {"x", 'x', 0},
     {"c", 'c', 0},
     {"v", 'v', 0},
     {"b", 'b', 0},
     {"n", 'n', 0},
     {"m", 'm', 0},
     {",", ',', 0},
     {".", '.', 0},
     {"/", '/', 0},
     },
    {
     {"OK", 0, DIALOG__KEY_OK},
     {"AB", 0, DIALOG__KEY_CAPS},
     {"<-", 0, DIALOG__KEY_DELETE},
     {"SP", 0, DIALOG__KEY_SPACE},
     {"X", 0, DIALOG__KEY_CANCEL},
     {NULL, 0, 0},
     {NULL, 0, 0},
     {NULL, 0, 0},
     {NULL, 0, 0},
     {NULL, 0, 0},
     },
};

#define DIALOG__HEX_COLS 4
#define DIALOG__HEX_ROWS 5

static const dialog__key_t s_hex_keys[DIALOG__HEX_ROWS][DIALOG__HEX_COLS] = {
    {{"0", '0', 0},             {"1", '1', 0},                  {"2", '2', 0},                  {"3", '3', 0}},
    {{"4", '4', 0},             {"5", '5', 0},                  {"6", '6', 0},                  {"7", '7', 0}},
    {{"8", '8', 0},             {"9", '9', 0},                  {"A", 'A', 0},                  {"B", 'B', 0}},
    {{"C", 'C', 0},             {"D", 'D', 0},                  {"E", 'E', 0},                  {"F", 'F', 0}},
    {{"OK", 0, DIALOG__KEY_OK}, {"DEL", 0, DIALOG__KEY_DELETE}, {"CAN", 0, DIALOG__KEY_CANCEL}, {NULL, 0, 0} },
};

#define DIALOG__NUM_COLS 3
#define DIALOG__NUM_ROWS 5

static const dialog__key_t s_num_keys[DIALOG__NUM_ROWS][DIALOG__NUM_COLS] = {
    {{"1", '1', 0},             {"2", '2', 0},                  {"3", '3', 0}                 },
    {{"4", '4', 0},             {"5", '5', 0},                  {"6", '6', 0}                 },
    {{"7", '7', 0},             {"8", '8', 0},                  {"9", '9', 0}                 },
    {{"-", '-', 0},             {"0", '0', 0},                  {".", '.', 0}                 },
    {{"OK", 0, DIALOG__KEY_OK}, {"DEL", 0, DIALOG__KEY_DELETE}, {"CAN", 0, DIALOG__KEY_CANCEL}},
};

typedef struct {
    const dialog__key_t *keys;
    int rows;
    int cols;
    int sel_row;
    int sel_col;
    bool caps;
    char *buffer;
    size_t buffer_size;
    size_t len;
    size_t max_len;
    bool mask_input;
    const char *title;
    const char *prompt;
} dialog__keyboard_state_t;

static bool dialog__key_is_valid(const dialog__key_t *key) { return key != NULL && key->label != NULL; }

static const dialog__key_t *dialog__current_key(const dialog__keyboard_state_t *st) {
    if (st->sel_row < 0 || st->sel_row >= st->rows || st->sel_col < 0 || st->sel_col >= st->cols) {
        return NULL;
    }
    return &st->keys[st->sel_row * st->cols + st->sel_col];
}

static void dialog__keyboard_find_valid_cell(dialog__keyboard_state_t *st, int dir_row, int dir_col) {
    int start_row = st->sel_row;
    int start_col = st->sel_col;
    for (int attempts = 0; attempts < st->rows * st->cols; ++attempts) {
        st->sel_row += dir_row;
        st->sel_col += dir_col;
        if (st->sel_row < 0) { st->sel_row = st->rows - 1; }
        if (st->sel_row >= st->rows) { st->sel_row = 0; }
        if (st->sel_col < 0) { st->sel_col = st->cols - 1; }
        if (st->sel_col >= st->cols) { st->sel_col = 0; }
        if (dialog__key_is_valid(dialog__current_key(st))) { return; }
        if (dir_row != 0 && dir_col != 0) {
            /* move horizontally first when both are requested */
            st->sel_row = start_row;
        }
    }
    /* restore if nothing found */
    st->sel_row = start_row;
    st->sel_col = start_col;
}

static void dialog__keyboard_ensure_valid(dialog__keyboard_state_t *st) {
    if (!dialog__key_is_valid(dialog__current_key(st))) { dialog__keyboard_find_valid_cell(st, 0, 1); }
}

static void dialog__keyboard_add_char(dialog__keyboard_state_t *st, char c) {
    if (st->len + 1 >= st->buffer_size || st->len >= st->max_len) { return; }
    st->buffer[st->len++] = c;
    st->buffer[st->len] = '\0';
}

static void dialog__keyboard_delete(dialog__keyboard_state_t *st) {
    if (st->len > 0) { st->buffer[--st->len] = '\0'; }
}

static bool dialog__keyboard_validate_char(dialog__keyboard_state_t *st, char c, int kind) {
    if (kind == BRUCE_DIALOG_INPUT_TEXT) { return (c >= 0x20 && c <= 0x7E) || c == ' '; }
    if (kind == BRUCE_DIALOG_INPUT_HEX) { return isxdigit((unsigned char)c) != 0; }
    if (kind == BRUCE_DIALOG_INPUT_NUMBER) {
        if (c == '-') { return st->len == 0; }
        if (c == '.') {
            for (size_t i = 0; i < st->len; ++i) {
                if (st->buffer[i] == '.') { return false; }
            }
            return true;
        }
        return isdigit((unsigned char)c) != 0;
    }
    return false;
}

static bruce_result_t dialog__keyboard_draw(dialog__keyboard_state_t *st, int kind) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) { return result; }
    int w = display__width();
    int h = display__height();

    int text_area_h = DIALOG__CHAR_H * 5 + 12;
    int keyboard_h = h - text_area_h;
    int cell_w = w / st->cols;
    int cell_h = keyboard_h / st->rows;
    if (cell_w < 1) { cell_w = 1; }
    if (cell_h < 1) { cell_h = 1; }

    dialog__gui_clear();
    dialog__gui_title_bar(st->title);

    /* Prompt and current text. */
    display__set_text_color(BRUCE_COLOR_WHITE);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_cursor(DIALOG__MARGIN, DIALOG__CHAR_H + 8);
    display__print(st->prompt != NULL ? st->prompt : "");

    /* Counter */
    char counter[32];
    snprintf(counter, sizeof(counter), "%zu/%zu", st->len, st->max_len);
    display__set_cursor(w - (int)strlen(counter) * DIALOG__CHAR_W - DIALOG__MARGIN, DIALOG__CHAR_H + 8);
    display__print(counter);

    /* Text box. */
    int textbox_y = DIALOG__CHAR_H * 2 + 10;
    display__draw_rect(
        DIALOG__MARGIN, textbox_y, w - 2 * DIALOG__MARGIN, DIALOG__CHAR_H * 2 + 4, BRUCE_COLOR_WHITE
    );
    display__set_cursor(DIALOG__MARGIN + 2, textbox_y + 2);

    if (st->mask_input) {
        char stars[64];
        size_t n = st->len < sizeof(stars) - 1 ? st->len : sizeof(stars) - 1;
        memset(stars, '*', n);
        stars[n] = '\0';
        display__print(stars);
    } else {
        int max_chars = (w - 4 * DIALOG__MARGIN) / DIALOG__CHAR_W;
        if (max_chars < 1) { max_chars = 1; }
        if ((int)st->len <= max_chars) {
            display__print(st->buffer);
        } else {
            display__print(st->buffer + st->len - max_chars);
        }
    }

    /* Keyboard grid. */
    for (int row = 0; row < st->rows; ++row) {
        for (int col = 0; col < st->cols; ++col) {
            const dialog__key_t *key = &st->keys[row * st->cols + col];
            if (!dialog__key_is_valid(key)) { continue; }

            int x = col * cell_w;
            int y = text_area_h + row * cell_h;

            bool selected = (row == st->sel_row && col == st->sel_col);
            const char *label = key->label;
            char tmp_label[2];
            if (!key->special && kind == BRUCE_DIALOG_INPUT_TEXT && st->caps &&
                isalpha((unsigned char)key->code)) {
                tmp_label[0] = (char)toupper((unsigned char)key->code);
                tmp_label[1] = '\0';
                label = tmp_label;
            }

            if (selected) {
                display__fill_rect(x, y, cell_w, cell_h, BRUCE_COLOR_WHITE);
                display__set_text_color(BRUCE_COLOR_BLACK);
            } else {
                display__draw_rect(x, y, cell_w, cell_h, BRUCE_COLOR_WHITE);
                display__set_text_color(BRUCE_COLOR_WHITE);
            }
            display__set_text_size(DIALOG__TEXT_SIZE);
            display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
            int label_w = (int)strlen(label) * DIALOG__CHAR_W;
            int label_x = x + (cell_w - label_w) / 2;
            if (label_x < x + 1) { label_x = x + 1; }
            int label_y = y + (cell_h - DIALOG__CHAR_H) / 2;
            if (label_y < y + 1) { label_y = y + 1; }
            display__set_cursor(label_x, label_y);
            display__print(label);
        }
    }

    return display__present();
}

static bruce_result_t dialog__gui_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size, int kind
) {
    const dialog__key_t *keys;
    int rows, cols;
    size_t max_len = buffer_size > 0 ? buffer_size - 1 : 0;

    if (kind == BRUCE_DIALOG_INPUT_TEXT) {
        keys = &s_text_keys[0][0];
        rows = DIALOG__TEXT_ROWS;
        cols = DIALOG__TEXT_COLS;
    } else if (kind == BRUCE_DIALOG_INPUT_HEX) {
        keys = &s_hex_keys[0][0];
        rows = DIALOG__HEX_ROWS;
        cols = DIALOG__HEX_COLS;
    } else {
        keys = &s_num_keys[0][0];
        rows = DIALOG__NUM_ROWS;
        cols = DIALOG__NUM_COLS;
    }

    dialog__keyboard_state_t st = {
        .keys = keys,
        .rows = rows,
        .cols = cols,
        .sel_row = 0,
        .sel_col = 0,
        .caps = false,
        .buffer = buffer,
        .buffer_size = buffer_size,
        .len = 0,
        .max_len = max_len,
        .mask_input = mask_input,
        .title = title,
        .prompt = prompt,
    };

    if (buffer_size > 0) { buffer[0] = '\0'; }
    if (initial_text != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "%s", initial_text);
        st.len = strlen(buffer);
        if (st.len > max_len) {
            st.len = max_len;
            buffer[st.len] = '\0';
        }
    }

    dialog__keyboard_ensure_valid(&st);

    /* See dialog__gui_choice()'s matching flush: discard whatever's still
     * queued from before this keyboard screen appeared. */
    (void)input__flush();
    for (;;) {
        bruce_result_t draw_result = dialog__keyboard_draw(&st, kind);
        if (draw_result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
        if (draw_result != BRUCE_OK) { return draw_result; }

        bruce_input_event_t ev;
        bruce_result_t input_result = input__read(&ev, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
        if (input_result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) { continue; }

        /* Physical Enter always submits the typed buffer, regardless of
         * where the on-screen grid cursor happens to be sitting.
         * BRUCE_INPUT_CODE_SELECT (0x00A) is defined to equal '\n' so a
         * dedicated physical keyboard's Enter key reuses the same code as a
         * D-pad/button "select" press (see core_sdk/input.h) - but the two
         * mean different things: a D-pad SELECT should activate whatever
         * grid cell is highlighted (handled below), while a keyboard user
         * who typed the whole buffer via the direct-typing fast path never
         * touched that cursor, so falling into the same grid-cell-activate
         * logic "presses" an arbitrary cell (often inserting a stray
         * character) instead of submitting - i.e. Enter appeared to do
         * nothing useful. Only intercept it here for real BRUCE_INPUT_KEY
         * events so BUTTON-sourced SELECT still drives the grid as before. */
        if (ev.type == BRUCE_INPUT_KEY && ev.code == '\n') { return BRUCE_OK; }

        /* Semantic navigation/action codes always win over raw typing, and
         * must be handled before the "direct typing" fallback below.
         * BRUCE_INPUT_CODE_UP/DOWN/LEFT/RIGHT/BACK deliberately alias
         * printable ASCII punctuation (';', '.', ',', '/', '`' - see
         * core_sdk/input.h) so that physical D-pad/button hardware can reuse
         * the same event codes as keyboard bindings. On keyboards that emit
         * navigation via an Fn-chord over those same punctuation keys (e.g.
         * Cardputer's Fn+;/./,// and the bare backtick for Back - see
         * input__kb_decode_fn_nav() in modules/input/input_keyboard.c), the
         * resulting event is byte-for-byte identical to the literal
         * character; there is no way to tell them apart downstream. Checking
         * this switch first means those five characters must be entered via
         * the on-screen grid instead of typed directly, but arrows/Back/OK
         * and Delete work the same way as every other dialog (dialog__gui_choice
         * included), instead of being silently swallowed as typed punctuation
         * before ever reaching this switch. */
        switch (ev.code) {
            case BRUCE_INPUT_CODE_UP:
            case BRUCE_INPUT_CODE_PREV: dialog__keyboard_find_valid_cell(&st, -1, 0); continue;
            case BRUCE_INPUT_CODE_DOWN:
            case BRUCE_INPUT_CODE_NEXT: dialog__keyboard_find_valid_cell(&st, 1, 0); continue;
            case BRUCE_INPUT_CODE_LEFT: dialog__keyboard_find_valid_cell(&st, 0, -1); continue;
            case BRUCE_INPUT_CODE_RIGHT: dialog__keyboard_find_valid_cell(&st, 0, 1); continue;
            case BRUCE_INPUT_CODE_SELECT:
            case BRUCE_INPUT_CODE_BUTTON_A: {
                const dialog__key_t *key = dialog__current_key(&st);
                if (key == NULL || key->label == NULL) { continue; }
                if (key->special == DIALOG__KEY_OK) { return BRUCE_OK; }
                if (key->special == DIALOG__KEY_CANCEL) { return BRUCE_ERR_CANCELLED; }
                if (key->special == DIALOG__KEY_DELETE) {
                    dialog__keyboard_delete(&st);
                } else if (key->special == DIALOG__KEY_SPACE) {
                    if (kind == BRUCE_DIALOG_INPUT_TEXT) { dialog__keyboard_add_char(&st, ' '); }
                } else if (key->special == DIALOG__KEY_CAPS) {
                    st.caps = !st.caps;
                } else if (key->code != '\0') {
                    char c = key->code;
                    if (kind == BRUCE_DIALOG_INPUT_TEXT && st.caps && isalpha((unsigned char)c)) {
                        c = (char)toupper((unsigned char)c);
                    }
                    if (dialog__keyboard_validate_char(&st, c, kind)) { dialog__keyboard_add_char(&st, c); }
                }
                continue;
            }
            case BRUCE_INPUT_CODE_BACK:
            case BRUCE_INPUT_CODE_BUTTON_B: return BRUCE_ERR_CANCELLED;
            /* Physical Backspace/Delete: a bare "del" key press (no Fn) emits
             * '\b' as a normal BRUCE_INPUT_KEY (see input__kb_char_code() in
             * input_keyboard.c); Fn+del emits the semantic
             * BRUCE_INPUT_CODE_DELETE instead. Neither was ever handled here
             * previously, so the physical key silently did nothing - the
             * on-screen "<-"/"DEL" grid key was the only way to delete. 0x7F
             * (ASCII DEL) is accepted too for keyboards/terminals that send
             * that instead of '\b'. */
            case '\b':
            case 0x7F:
            case BRUCE_INPUT_CODE_DELETE: dialog__keyboard_delete(&st); continue;
            default: break;
        }

        /* Direct physical keyboard typing for text modes: only reached for
         * codes the switch above didn't claim as navigation/action/delete. */
        if (kind == BRUCE_DIALOG_INPUT_TEXT && ev.type == BRUCE_INPUT_KEY && ev.code >= 0x20 &&
            ev.code <= 0x7E) {
            dialog__keyboard_add_char(&st, (char)ev.code);
            continue;
        }
        if (kind == BRUCE_DIALOG_INPUT_HEX && ev.type == BRUCE_INPUT_KEY &&
            isxdigit((unsigned char)ev.code)) {
            dialog__keyboard_add_char(&st, (char)ev.code);
            continue;
        }
        if (kind == BRUCE_DIALOG_INPUT_NUMBER && ev.type == BRUCE_INPUT_KEY &&
            dialog__keyboard_validate_char(&st, (char)ev.code, kind)) {
            dialog__keyboard_add_char(&st, (char)ev.code);
            continue;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* GUI file picker                                                            */
/* -------------------------------------------------------------------------- */

static bool dialog__matches_extension_filter(const char *name, const char *extension_filter) {
    if (extension_filter == NULL || extension_filter[0] == '\0') { return true; }
    size_t name_len = strlen(name);
    size_t filter_len = strlen(extension_filter);
    if (filter_len == 0 || name_len < filter_len) { return false; }
    return strcasecmp(name + name_len - filter_len, extension_filter) == 0;
}

static int dialog__compare_file_picker_entries(const void *left, const void *right) {
    const bruce_storage_entry_t *left_entry = left;
    const bruce_storage_entry_t *right_entry = right;
    if (left_entry->type != right_entry->type) {
        return left_entry->type == BRUCE_STORAGE_ENTRY_DIRECTORY ? -1 : 1;
    }
    return strcasecmp(left_entry->name, right_entry->name);
}

/* True if the picker's choice dialog was cancelled because the process lost
 * (and has now regained) foreground - e.g. the user alt-tabbed away and
 * back - rather than a genuine Back/Esc press. Blocks until foreground
 * returns so the caller can redraw the same listing instead of unwinding. */
static bool dialog__pick_file_resume_after_handoff(void) {
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

/* Strips the last path component in place, e.g. "/a/b" -> "/a", "/a" -> "/". */
static void dialog__pick_file_go_up(char *current_path) {
    char *last_slash = strrchr(current_path, '/');
    if (last_slash != NULL && last_slash != current_path) {
        *last_slash = '\0';
    } else {
        current_path[1] = '\0';
    }
}

/* Copies `current_path`'s last path component (the directory about to be
 * left via ".."/Back) into `out_name`, so the next loop iteration - once it
 * relists the parent - can re-select that entry instead of defaulting back
 * to the top. Only ever holds the single most recent directory left, not a
 * full history stack, so this re-selection only reaches one level up. */
static void dialog__pick_file_note_returning_from(const char *current_path, char *out_name, size_t out_name_size) {
    const char *last_slash = strrchr(current_path, '/');
    snprintf(out_name, out_name_size, "%s", last_slash != NULL ? last_slash + 1 : current_path);
}

/* Whole-number binary-prefix size, e.g. 20480 -> "20KiB", 2097152 -> "2MiB".
 * Truncates rather than rounding: plenty precise for a status-bar readout. */
static void dialog__pick_file_format_bytes(uint64_t bytes, char *out, size_t out_size) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    size_t unit = 0;
    uint64_t divisor = 1;
    while (unit + 1 < sizeof(units) / sizeof(units[0]) && bytes >= divisor * 1024) {
        divisor *= 1024;
        unit++;
    }
    snprintf(out, out_size, "%llu%s", (unsigned long long)(bytes / divisor), units[unit]);
}

/* Fills `out` with "sd" under the SD card mount, or the label of whichever
 * internal partition is mounted on the longest matching path prefix of
 * `path` - covers every extra partition created via bparted
 * (storage__mount_partition()), not just the root "littlefs" volume, which
 * a fixed "sd"-or-"littlefs" guess got wrong for anything mounted
 * elsewhere. Falls back to the root label if nothing else matches (it
 * always should, since root mounts at "/"). */
static void dialog__pick_file_partition_label(const char *path, char *out, size_t out_size) {
    size_t sd_len = strlen(STORAGE__SD_MOUNT_PATH);
    if (strncmp(path, STORAGE__SD_MOUNT_PATH, sd_len) == 0 && (path[sd_len] == '\0' || path[sd_len] == '/')) {
        snprintf(out, out_size, "sd");
        return;
    }
    snprintf(out, out_size, "%s", BRUCE_PARTITION_ROOT_LABEL);

    /* Heap, not stack: BRUCE_PARTITION_MAX_ENTRIES worth of
     * bruce_partition_entry_t plus a path-sized mount-point buffer add up to
     * several hundred bytes, and this runs on the picker's redraw path - see
     * dialog__pick_file_workspace_t below for why that path stays off the
     * caller's stack. */
    bruce_partition_entry_t *entries = memory__malloc(BRUCE_PARTITION_MAX_ENTRIES * sizeof(*entries));
    char *mount_point = memory__malloc(BRUCE_STORAGE_PATH_MAX);
    if (entries == NULL || mount_point == NULL) {
        memory__free(entries);
        memory__free(mount_point);
        return;
    }

    size_t count = 0;
    size_t best_len = 0;
    if (partition_manager__list_current(entries, BRUCE_PARTITION_MAX_ENTRIES, &count) == BRUCE_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (!storage__internal_mount_point(entries[i].label, mount_point, BRUCE_STORAGE_PATH_MAX)) continue;
            size_t mount_len = strlen(mount_point);
            bool root_mount = mount_len == 1 && mount_point[0] == '/';
            bool matches = strncmp(path, mount_point, mount_len) == 0 &&
                           (root_mount || path[mount_len] == '\0' || path[mount_len] == '/');
            if (matches && mount_len >= best_len) {
                best_len = mount_len;
                snprintf(out, out_size, "%s", entries[i].label);
            }
        }
    }
    memory__free(entries);
    memory__free(mount_point);
}

/* "littlefs 20KiB/2MiB"-style summary of the volume `path` lives on, for the
 * bottom bar. Leaves `out` empty if usage can't be read. */
static void dialog__pick_file_format_usage(const char *path, char *out, size_t out_size) {
    size_t total = 0, used = 0;
    if (out_size == 0) return;
    if (storage__get_usage(path, &total, &used) != BRUCE_OK) {
        out[0] = '\0';
        return;
    }
    char used_text[16];
    char total_text[16];
    char label[BRUCE_PARTITION_LABEL_MAX];
    dialog__pick_file_format_bytes(used, used_text, sizeof(used_text));
    dialog__pick_file_format_bytes(total, total_text, sizeof(total_text));
    dialog__pick_file_partition_label(path, label, sizeof(label));
    snprintf(out, out_size, "%s %s/%s", label, used_text, total_text);
}

/* render_callback for the picker's own render_params: right-aligns the
 * usage summary (context = a `const char *` built by
 * dialog__pick_file_format_usage() above) into the bottom bar dialog__gui_choice()
 * just filled. Assumes the plain full-screen bordered layout (the only one
 * any current caller uses for the picker) - silently draws nothing usable
 * outside that when the text doesn't fit, rather than fail the dialog. */
static void dialog__pick_file_draw_footer_usage(void *context) {
    const char *text = context;
    if (text == NULL || text[0] == '\0') return;
    uint16_t pri, sec, bg, surface, txt, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &txt, &text_muted, &border, &success, &warning, &error);
    (void)pri;
    (void)bg;
    (void)surface;
    (void)text_muted;
    (void)border;
    (void)success;
    (void)warning;
    (void)error;
    int footer_h = DIALOG__CHAR_H + 4;
    display__set_text_color(txt);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(sec);
    display__draw_right_string(
        text, display__width() - DIALOG__MARGIN, display__height() - footer_h + DIALOG__MARGIN
    );
}

/* Every sizable buffer dialog__gui_pick_file() needs, bundled into one
 * heap allocation instead of individual locals. The picker is invoked from
 * whichever task called dialog__pick_file[_ex]() - the file manager, an IR
 * app, a JS/ELF/WASM guest, ... - and stacking ~750 bytes of these on top of
 * that caller's own frames (plus dialog__gui_choice()'s below them) is
 * exactly the kind of per-caller cost a shared task stack size can't
 * absorb; a stack overflow here means every task that ever calls into the
 * picker would need a bigger stack just to cover it. One malloc/free pair
 * around the loop keeps that cost off every caller's stack instead. */
typedef struct {
    char current_path[BRUCE_STORAGE_PATH_MAX];
    /* Name of the directory/file last left via ".."/Back, so it can be
     * re-highlighted once its parent's listing is rebuilt below. */
    char returning_from[BRUCE_STORAGE_NAME_MAX];
    char next_path[BRUCE_STORAGE_PATH_MAX];
    char bar_title[BRUCE_STORAGE_PATH_MAX + 32];
    /* "<label> <used>/<total>", e.g. "littlefs 20KiB/2MiB" - sized off
     * BRUCE_PARTITION_LABEL_MAX (a label can now be any bparted partition
     * name, not just the fixed "sd"/"littlefs" guess) plus two
     * dialog__pick_file_format_bytes() outputs and their separators. */
    char usage_text[BRUCE_PARTITION_LABEL_MAX + 2 * 16 + 4];
} dialog__pick_file_workspace_t;

static bruce_result_t dialog__gui_pick_file_run(
    dialog__pick_file_workspace_t *ws, const char *initial_path, const char *extension_filter, char *out_path,
    size_t out_path_size, const char *title, bruce_dialog_render_params_t *effective_params
) {
    snprintf(
        ws->current_path,
        sizeof(ws->current_path),
        "%s",
        initial_path != NULL && initial_path[0] != '\0' ? initial_path : "/"
    );
    ws->returning_from[0] = '\0';

    /* A caller (e.g. the file manager, reopening the browser after an
     * Esc/action out of a file it just picked) may pass a *file* path here
     * instead of a directory to list. storage__list() can't list a file, so
     * detect that up front and fall back to browsing its parent with that
     * file pre-selected, the same way stepping back up out of a directory
     * pre-selects it below. */
    size_t initial_list_count = 0;
    if (storage__list(ws->current_path, NULL, 0, &initial_list_count) != BRUCE_OK) {
        dialog__pick_file_note_returning_from(ws->current_path, ws->returning_from, sizeof(ws->returning_from));
        dialog__pick_file_go_up(ws->current_path);
    }
    (void)initial_list_count;

    for (;;) {
        size_t count = 0;
        bruce_result_t list_result = storage__list(ws->current_path, NULL, 0, &count);
        if (list_result != BRUCE_OK) { return list_result; }

        bruce_storage_entry_t *entries = memory__malloc(count * sizeof(bruce_storage_entry_t));
        if (entries == NULL && count > 0) { return BRUCE_ERR_NO_MEMORY; }
        if (entries != NULL) {
            list_result = storage__list(ws->current_path, entries, count, &count);
            if (list_result != BRUCE_OK) {
                memory__free(entries);
                return list_result;
            }
            qsort(entries, count, sizeof(*entries), dialog__compare_file_picker_entries);
        }

        int h = display__height();
        int usable_h = h - (DIALOG__CHAR_H + 4) - (DIALOG__CHAR_H + 4);
        int items_per_page = usable_h / (DIALOG__CHAR_H + 2);
        if (items_per_page < 1) { items_per_page = 1; }
        (void)items_per_page;

        bruce_dialog_choice_t *choices = memory__calloc(count + 1, sizeof(bruce_dialog_choice_t));
        if (choices == NULL) {
            memory__free(entries);
            return BRUCE_ERR_NO_MEMORY;
        }
        const char **values = memory__malloc((count + 1) * sizeof(const char *));
        if (values == NULL) {
            memory__free(entries);
            memory__free(choices);
            return BRUCE_ERR_NO_MEMORY;
        }

        int choice_count = 0;
        /* ".." entry to go up, except at root. */
        if (strcmp(ws->current_path, "/") != 0) {
            values[choice_count] = "..";
            choices[choice_count].label = "[..]";
            choices[choice_count].value = "..";
            choices[choice_count].icon_name = "folder-open";
            choice_count++;
        }
        for (size_t i = 0; i < count && (size_t)choice_count < count + 1; ++i) {
            if (entries[i].type == BRUCE_STORAGE_ENTRY_FILE &&
                !dialog__matches_extension_filter(entries[i].name, extension_filter)) {
                continue;
            }
            values[choice_count] = entries[i].name;
            choices[choice_count].label = entries[i].name;
            choices[choice_count].value = entries[i].name;
            choices[choice_count].icon_name = entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY
                                                   ? "folder"
                                                   : app_runner__icon_for_path(entries[i].name);
            choice_count++;
        }

        size_t out_selected = 0;
        if (ws->returning_from[0] != '\0') {
            for (int i = 0; i < choice_count; ++i) {
                if (strcmp(values[i], ws->returning_from) == 0) {
                    out_selected = (size_t)i;
                    break;
                }
            }
            /* Deliberately left set rather than cleared here: opening a file
             * (Open/Open with... an image or JS app, say) hands the display
             * to a foreground child process, and dialog__gui_choice() below
             * can lose - and immediately regain - foreground before the
             * child even runs, which the branch below treats as a handoff
             * and redraws this same directory via `continue`. Clearing here
             * would drop the pre-selection on that very first redraw, before
             * the user ever saw it. It's overwritten fresh on the next
             * genuine ".."/Back and cleared on actually descending, so it
             * never leaks into an unrelated directory. */
        }

        if (title != NULL && title[0] != '\0') {
            snprintf(ws->bar_title, sizeof(ws->bar_title), "%s - %s", title, ws->current_path);
        } else {
            snprintf(ws->bar_title, sizeof(ws->bar_title), "%s", ws->current_path);
        }
        dialog__pick_file_format_usage(ws->current_path, ws->usage_text, sizeof(ws->usage_text));
        effective_params->render_callback_context = ws->usage_text;

        bruce_result_t choice_result = dialog__gui_choice(
            ws->bar_title, NULL, choices, (size_t)choice_count, &out_selected, effective_params
        );

        const char *picked = values[out_selected];
        memory__free(values);
        memory__free(choices);

        if (choice_result != BRUCE_OK) {
            memory__free(entries);
            /* Foreground was lost (e.g. alt-tab) and has now returned:
             * redraw the same directory instead of unwinding the picker. */
            if (dialog__pick_file_resume_after_handoff()) { continue; }
            /* Genuine Back/Esc: step up a directory rather than exiting the
             * picker outright, unless already at the root. */
            if (strcmp(ws->current_path, "/") != 0) {
                dialog__pick_file_note_returning_from(ws->current_path, ws->returning_from, sizeof(ws->returning_from));
                dialog__pick_file_go_up(ws->current_path);
                continue;
            }
            return BRUCE_ERR_CANCELLED;
        }

        if (strcmp(picked, "..") == 0) {
            dialog__pick_file_note_returning_from(ws->current_path, ws->returning_from, sizeof(ws->returning_from));
            dialog__pick_file_go_up(ws->current_path);
            memory__free(entries);
            continue;
        }

        int printed;
        if (strcmp(ws->current_path, "/") == 0) {
            printed = snprintf(ws->next_path, sizeof(ws->next_path), "/%s", picked);
        } else {
            printed = snprintf(ws->next_path, sizeof(ws->next_path), "%s/%s", ws->current_path, picked);
        }
        if (printed < 0 || (size_t)printed >= sizeof(ws->next_path)) {
            memory__free(entries);
            return BRUCE_ERR_INVALID_PATH;
        }

        /* If the picked entry is a directory, descend into it. */
        bool is_file = false;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(entries[i].name, picked) == 0) {
                is_file = (entries[i].type == BRUCE_STORAGE_ENTRY_FILE);
                break;
            }
        }

        if (is_file) {
            snprintf(out_path, out_path_size, "%s", ws->next_path);
            memory__free(entries);
            return BRUCE_OK;
        }

        snprintf(ws->current_path, sizeof(ws->current_path), "%s", ws->next_path);
        ws->returning_from[0] = '\0';
        memory__free(entries);
    }
}

static bruce_result_t dialog__gui_pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title, const bruce_dialog_render_params_t *render_params
) {
    /* Own the render_callback slot for the bottom-bar usage summary (see
     * dialog__pick_file_draw_footer_usage()); everything else in
     * `render_params` (padding, colors, text size, ...) passes through
     * unchanged, defaulting the same way a NULL render_params would. Also
     * own list_gap: only the picker's own listing wants breathing room
     * below its title bar, not every dialog__choice()/_ex() caller. */
    bruce_dialog_render_params_t effective_params =
        render_params != NULL ? *render_params : dialog__default_render_params(0);
    effective_params.render_callback = dialog__pick_file_draw_footer_usage;
    effective_params.list_gap = DIALOG__LIST_GAP;

    dialog__pick_file_workspace_t *ws = memory__malloc(sizeof(*ws));
    if (ws == NULL) { return BRUCE_ERR_NO_MEMORY; }
    bruce_result_t result = dialog__gui_pick_file_run(
        ws, initial_path, extension_filter, out_path, out_path_size, title, &effective_params
    );
    memory__free(ws);
    return result;
}

/* -------------------------------------------------------------------------- */
/* Text viewer resource management                                            */
/* -------------------------------------------------------------------------- */

#define DIALOG__VIEWER_MAX 4

typedef struct {
    bool used;
    bruce_process_id_t owner;
    bruce_resource_id_t resource_id;
    char *title;
    char *text;
    int scroll_y;
    int text_size;
} dialog__viewer_t;

static dialog__viewer_t s_viewers[DIALOG__VIEWER_MAX];
static StaticSemaphore_t s_viewer_mutex_storage;
static SemaphoreHandle_t s_viewer_mutex;

static void dialog__viewer_lock(void) {
    if (s_viewer_mutex != NULL) { xSemaphoreTake(s_viewer_mutex, portMAX_DELAY); }
}

static void dialog__viewer_unlock(void) {
    if (s_viewer_mutex != NULL) { xSemaphoreGive(s_viewer_mutex); }
}

static char *dialog__strdup(const char *src) {
    if (src == NULL) { src = ""; }
    size_t len = strlen(src);
    char *copy = memory__malloc(len + 1);
    if (copy == NULL) { return NULL; }
    memcpy(copy, src, len + 1);
    return copy;
}

static dialog__viewer_t *dialog__viewer_find(bruce_viewer_id_t id) {
    if (id == BRUCE_VIEWER_ID_INVALID || id > DIALOG__VIEWER_MAX) { return NULL; }
    dialog__viewer_t *viewer = &s_viewers[id - 1];
    return viewer->used ? viewer : NULL;
}

static void dialog__viewer_free(dialog__viewer_t *viewer) {
    if (viewer == NULL) { return; }
    memory__free(viewer->title);
    memory__free(viewer->text);
    viewer->title = NULL;
    viewer->text = NULL;
    viewer->used = false;
    viewer->owner = BRUCE_PROCESS_ID_INVALID;
    viewer->resource_id = BRUCE_RESOURCE_ID_INVALID;
    viewer->scroll_y = 0;
    viewer->text_size = DIALOG__TEXT_SIZE;
}

static void dialog__viewer_cleanup(void *context) {
    dialog__viewer_t *viewer = (dialog__viewer_t *)context;
    dialog__viewer_lock();
    dialog__viewer_free(viewer);
    dialog__viewer_unlock();
}

static bruce_result_t dialog__viewer_draw(dialog__viewer_t *viewer, bool gui) {
    if (!gui) {
        stdio__printf("--- %s ---\n", viewer->title != NULL ? viewer->title : "viewer");
        stdio__printf("%s\n", viewer->text != NULL ? viewer->text : "");
        stdio__printf("---\n");
        return BRUCE_OK;
    }

    bruce_result_t frame_result = display__begin_frame();
    if (frame_result != BRUCE_OK) {
        return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
    }
    int w = display__width();
    int h = display__height();
    int body_y = DIALOG__CHAR_H + 6;
    int usable_h = h - body_y - (DIALOG__CHAR_H + 4);
    int lines_per_screen = usable_h / (DIALOG__CHAR_H * viewer->text_size + 1);
    if (lines_per_screen < 1) { lines_per_screen = 1; }
    int max_chars = (w - 2 * DIALOG__MARGIN) / (DIALOG__CHAR_W * viewer->text_size);
    if (max_chars < 1) { max_chars = 1; }

    dialog__gui_clear();
    dialog__gui_title_bar(viewer->title);

    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    (void)pri;
    (void)sec;
    (void)surface;
    (void)text_muted;
    (void)border;
    (void)success;
    (void)warning;
    (void)error;
    display__set_text_color(text);
    display__set_text_size((uint8_t)viewer->text_size);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);

    int y = body_y;
    int line = 0;
    int drawn = 0;
    const char *p = viewer->text != NULL ? viewer->text : "";
    while (*p != '\0' && drawn < lines_per_screen) {
        if (line >= viewer->scroll_y) {
            display__set_cursor(DIALOG__MARGIN, y);
            int col = 0;
            while (*p != '\0' && *p != '\n' && col < max_chars) {
                size_t bytes = (unsigned char)*p < 0x80 ? 1 : ((unsigned char)*p >= 0xF0 ? 4 : (unsigned char)*p >= 0xE0 ? 3 : 2);
                char ch[5] = {0};
                memcpy(ch, p, bytes);
                display__print(ch);
                col++;
                p += bytes;
            }
            y += DIALOG__CHAR_H * viewer->text_size + 1;
            drawn++;
        } else {
            while (*p != '\0' && *p != '\n') { p++; }
        }
        if (*p == '\n') { p++; }
        line++;
    }

    char footer[32];
    int total_lines = 0;
    const char *q = viewer->text != NULL ? viewer->text : "";
    while (*q != '\0') {
        if (*q == '\n') { total_lines++; }
        q++;
    }
    snprintf(footer, sizeof(footer), "%d/%d", viewer->scroll_y + 1, total_lines + 1);
    dialog__gui_footer(footer);
    frame_result = display__present();
    if (frame_result != BRUCE_OK) {
        return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
    }
    return BRUCE_OK;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (gui) { return dialog__gui_message(kind, title, message); }
    return dialog__term_message(kind, title, message);
}

bruce_result_t dialog__choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    if (choices == NULL || choice_count == 0 || out_selected == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_choice_provider != NULL) {
        return s_test_choice_provider(title, message, choices, choice_count, out_selected);
    }

    if (gui) {
        bruce_dialog_render_params_t params = dialog__default_render_params(2);
        params.padding_top = 8;
        params.padding_bottom = 8;
        params.padding_left = 12;
        params.padding_right = 12;
        return dialog__gui_choice(title, message, choices, choice_count, out_selected, &params);
    }
    return dialog__term_choice(title, message, choices, choice_count, out_selected);
}

static const bruce_dialog_render_params_t s_window_launcher = {.render_launcher = true};
bruce_result_t dialog__choice_launcher(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    if (choices == NULL || choice_count == 0 || out_selected == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_choice_provider != NULL) {
        return s_test_choice_provider(title, message, choices, choice_count, out_selected);
    }

    if (gui) {
        return dialog__gui_choice(title, message, choices, choice_count, out_selected, &s_window_launcher);
    }
    return dialog__term_choice(title, message, choices, choice_count, out_selected);
}

bruce_result_t dialog__choice_ex(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected, const bruce_dialog_render_params_t *render_params
) {
    if (choices == NULL || choice_count == 0 || out_selected == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (gui && render_params != NULL &&
        (render_params->padding_top < 0 || render_params->padding_right < 0 ||
         render_params->padding_bottom < 0 || render_params->padding_left < 0 ||
         render_params->padding_left + render_params->padding_right >= display__width() ||
         render_params->padding_top + render_params->padding_bottom >= display__height())) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    if (s_test_choice_provider != NULL) {
        return s_test_choice_provider(title, message, choices, choice_count, out_selected);
    }

    if (gui) {
        return dialog__gui_choice(title, message, choices, choice_count, out_selected, render_params);
    }
    return dialog__term_choice(title, message, choices, choice_count, out_selected);
}

bruce_result_t dialog__pick_file(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title
) {
    return dialog__pick_file_ex(initial_path, extension_filter, out_path, out_path_size, title, NULL);
}

bruce_result_t dialog__pick_file_ex(
    const char *initial_path, const char *extension_filter, char *out_path, size_t out_path_size,
    const char *title, const bruce_dialog_render_params_t *render_params
) {
    if (out_path == NULL || out_path_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_pick_file_provider != NULL) {
        return s_test_pick_file_provider(initial_path, extension_filter, out_path, out_path_size);
    }

    if (gui) {
        return dialog__gui_pick_file(initial_path, extension_filter, out_path, out_path_size, title, render_params);
    }
    return dialog__term_pick_file(initial_path, extension_filter, out_path, out_path_size);
}

bruce_result_t dialog__text_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_input_provider != NULL) {
        return s_test_input_provider(title, prompt, initial_text, mask_input, buffer, buffer_size);
    }

    if (gui) {
        return dialog__gui_input(
            title, prompt, initial_text, mask_input, buffer, buffer_size, BRUCE_DIALOG_INPUT_TEXT
        );
    }
    return dialog__term_input(title, prompt, initial_text, mask_input, buffer, buffer_size, NULL);
}

bruce_result_t dialog__hex_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_input_provider != NULL) {
        return s_test_input_provider(title, prompt, initial_text, false, buffer, buffer_size);
    }

    if (gui) {
        return dialog__gui_input(
            title, prompt, initial_text, false, buffer, buffer_size, BRUCE_DIALOG_INPUT_HEX
        );
    }
    return dialog__term_input(title, prompt, initial_text, false, buffer, buffer_size, dialog__validate_hex);
}

bruce_result_t dialog__number_input(
    const char *title, const char *prompt, const char *initial_text, char *buffer, size_t buffer_size
) {
    if (buffer == NULL || buffer_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;

    if (s_test_input_provider != NULL) {
        return s_test_input_provider(title, prompt, initial_text, false, buffer, buffer_size);
    }

    if (gui) {
        return dialog__gui_input(
            title, prompt, initial_text, false, buffer, buffer_size, BRUCE_DIALOG_INPUT_NUMBER
        );
    }
    return dialog__term_input(
        title, prompt, initial_text, false, buffer, buffer_size, dialog__validate_number
    );
}

bruce_result_t
dialog__create_text_viewer(const char *title, const char *text, bruce_viewer_id_t *out_viewer) {
    if (out_viewer == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    *out_viewer = BRUCE_VIEWER_ID_INVALID;

    dialog__viewer_lock();
    if (s_viewer_mutex == NULL) { s_viewer_mutex = xSemaphoreCreateMutexStatic(&s_viewer_mutex_storage); }

    dialog__viewer_t *slot = NULL;
    for (int i = 0; i < DIALOG__VIEWER_MAX; ++i) {
        if (!s_viewers[i].used) {
            slot = &s_viewers[i];
            break;
        }
    }
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    slot->used = true;
    slot->owner = process__current_id();
    slot->title = title != NULL ? dialog__strdup(title) : dialog__strdup("");
    slot->text = text != NULL ? dialog__strdup(text) : dialog__strdup("");
    slot->scroll_y = 0;
    slot->text_size = DIALOG__TEXT_SIZE;
    slot->resource_id = BRUCE_RESOURCE_ID_INVALID;

    if (slot->title == NULL || slot->text == NULL) {
        dialog__viewer_free(slot);
        dialog__viewer_unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    bruce_viewer_id_t id = (bruce_viewer_id_t)(slot - s_viewers + 1);
    slot->resource_id = process_registry__resource_register(dialog__viewer_cleanup, slot);

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;
    dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    *out_viewer = id;
    return BRUCE_OK;
}

bruce_result_t dialog__viewer_set_text(bruce_viewer_id_t viewer, const char *text) {
    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    char *copy = text != NULL ? dialog__strdup(text) : dialog__strdup("");
    if (copy == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    memory__free(slot->text);
    slot->text = copy;
    slot->scroll_y = 0;

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;
    bruce_result_t result = dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    return result;
}

bruce_result_t dialog__viewer_scroll(bruce_viewer_id_t viewer, int lines) {
    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    slot->scroll_y += lines;
    if (slot->scroll_y < 0) { slot->scroll_y = 0; }

    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;
    bruce_result_t result = dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    return result;
}

bruce_result_t dialog__viewer_set_text_size(bruce_viewer_id_t viewer, int text_size) {
    if (text_size < 1 || text_size > 8) return BRUCE_ERR_INVALID_ARGUMENT;

    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    slot->text_size = text_size;
    bool gui = dialog__current_process_wants_gui();
    s_last_call_was_gui = gui;
    bruce_result_t result = dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    return result;
}

bruce_result_t dialog__viewer_close(bruce_viewer_id_t viewer) {
    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    if (slot->resource_id != BRUCE_RESOURCE_ID_INVALID) {
        process_registry__resource_release(slot->resource_id);
    }
    dialog__viewer_free(slot);
    dialog__viewer_unlock();
    return BRUCE_OK;
}
