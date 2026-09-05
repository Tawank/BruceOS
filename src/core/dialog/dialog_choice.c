#include "dialog_choice.h"

#include "dialog_gui_common.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/display.h"
#include "core_sdk/icon.h"
#include "core_sdk/input.h"
#include "core_sdk/runtime.h"

/* Mirrors the active font's cell size (display__get_font_metrics()) as
 * compile-time constants -- every built-in font is monospace 6x10 today. */
#define DIALOG__CHAR_W 6
#define DIALOG__CHAR_H 10
#define DIALOG__TEXT_SIZE 1
#define DIALOG__MARGIN 2
#define DIALOG__WINDOW_RADIUS 5
#define DIALOG__WINDOW_INSET 4
#define DIALOG__OVERFLOW_MARKER ">"
#define DIALOG__MARQUEE_INITIAL_DELAY_MS 700u
#define DIALOG__MARQUEE_STEP_MS 180u
#define DIALOG__MARQUEE_END_DELAY_MS 700u
/* How long a SELECT press must be held before dialog__gui_choice() resolves
 * it as a long press instead of an instant selection - see
 * bruce_dialog_render_params_t.long_press_enabled. */
#define DIALOG__LONG_PRESS_MS 500u
/* Width of, and gap before, the scrollbar dialog__gui_choice() draws down the
 * right edge of a list that doesn't fit on one page. */
#define DIALOG__SCROLLBAR_W 4
#define DIALOG__SCROLLBAR_GAP 3

/* -------------------------------------------------------------------------- */
/* Window renderer registry                                                   */
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

/* Blocks until the SELECT press dialog__gui_choice() just consumed either
 * releases or has been held DIALOG__LONG_PRESS_MS, whichever comes first -
 * distinguishing a long press from a plain tap for a long_press_enabled
 * caller. `press_started_at` and the checks below both use runtime__now();
 * input event timestamps use the FreeRTOS tick epoch and cannot safely be
 * compared with runtime__now()'s ESP timer epoch. Only reached when a caller
 * opts in, so every existing dialog__choice()/_ex() caller keeps returning
 * on the raw PRESS exactly as before this existed. */
static bool dialog__gui_wait_long_press(uint64_t press_started_at) {
    for (;;) {
        if (runtime__now() - press_started_at >= DIALOG__LONG_PRESS_MS) { return true; }
        bruce_input_event_t ev;
        bruce_result_t result = input__read(&ev, 20);
        if (result == BRUCE_ERR_NOT_FOREGROUND) { return false; }
        if (result == BRUCE_OK && ev.action == BRUCE_INPUT_RELEASE && ev.code == BRUCE_INPUT_CODE_SELECT) {
            return false;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Row label rendering                                                        */
/* -------------------------------------------------------------------------- */

static size_t dialog__utf8_bytes(const char *text, size_t remaining) {
    unsigned char c = (unsigned char)text[0];
    size_t bytes = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2);
    return bytes <= remaining ? bytes : 1;
}

static size_t dialog__utf8_columns(const char *text) {
    size_t length = strlen(text);
    size_t columns = 0;
    for (size_t offset = 0; offset < length; columns++) {
        offset += dialog__utf8_bytes(text + offset, length - offset);
    }
    return columns;
}

static size_t dialog__utf8_offset(const char *text, size_t wanted_column) {
    size_t length = strlen(text);
    size_t offset = 0;
    for (size_t column = 0; offset < length && column < wanted_column; column++) {
        offset += dialog__utf8_bytes(text + offset, length - offset);
    }
    return offset;
}

static void dialog__copy_utf8_columns(
    char *output, size_t capacity, const char *text, size_t first_column, size_t column_count
) {
    size_t text_length = strlen(text);
    size_t start = dialog__utf8_offset(text, first_column);
    size_t end = start;
    for (size_t column = 0; end < text_length && column < column_count; column++) {
        size_t bytes = dialog__utf8_bytes(text + end, text_length - end);
        if (end - start + bytes >= capacity) break;
        end += bytes;
    }
    size_t length = end - start;
    memcpy(output, text + start, length);
    output[length] = 0;
}

/* The display text API wraps at the viewport edge. Idle overflowing rows use
 * one right-pointing marker; the selected row pauses, scrolls to its end,
 * pauses there, and repeats so every UTF-8 column can be read. */
static void dialog__gui_draw_row_label(
    const char *label, int x, int y, int max_width, int text_size, bool selected, uint64_t selected_for_ms,
    bool *out_overflow
) {
    if (label == NULL || max_width <= 0) { return; }
    int max_chars = max_width / (DIALOG__CHAR_W * text_size);
    if (max_chars <= 0) { return; }

    size_t columns = dialog__utf8_columns(label);
    if (columns <= (size_t)max_chars) {
        display__set_cursor(x, y);
        display__print(label);
        return;
    }
    if (out_overflow != NULL) *out_overflow = true;

    char rendered[256];
    if (!selected) {
        size_t visible_columns = max_chars > 1 ? (size_t)max_chars - 1u : 0u;
        dialog__copy_utf8_columns(rendered, sizeof(rendered), label, 0, visible_columns);
        size_t length = strlen(rendered);
        snprintf(rendered + length, sizeof(rendered) - length, "%s", DIALOG__OVERFLOW_MARKER);
        display__set_cursor(x, y);
        display__print(rendered);
        return;
    }

    size_t maximum_offset = columns - (size_t)max_chars;
    uint64_t cycle_ms = DIALOG__MARQUEE_INITIAL_DELAY_MS + maximum_offset * DIALOG__MARQUEE_STEP_MS +
                        DIALOG__MARQUEE_END_DELAY_MS;
    uint64_t phase_ms = cycle_ms > 0 ? selected_for_ms % cycle_ms : 0;
    size_t first_column = 0;
    if (phase_ms > DIALOG__MARQUEE_INITIAL_DELAY_MS) {
        uint64_t scrolling_ms = phase_ms - DIALOG__MARQUEE_INITIAL_DELAY_MS;
        first_column = (size_t)(scrolling_ms / DIALOG__MARQUEE_STEP_MS) + 1u;
        if (first_column > maximum_offset) first_column = maximum_offset;
    }
    dialog__copy_utf8_columns(rendered, sizeof(rendered), label, first_column, (size_t)max_chars);
    display__set_cursor(x, y);
    display__print(rendered);
}

/* Thin vertical scrollbar for a list that scrolls: a track from (x, y) down
 * `h` px, with a thumb sized to the visible fraction of `total` rows and
 * positioned at `window_start`'s fraction of the scrollable range. Mirrors
 * bruce_launcher__draw_scrollbar()'s look so a list dialog and the launcher's
 * own list layout read as the same widget. No-ops when everything already
 * fits (total <= visible). */
static void dialog__gui_draw_scrollbar(
    int x, int y, int h, int window_start, int visible, int total, uint16_t track_color, uint16_t thumb_color
) {
    if (total <= visible || h <= 4 || visible <= 0) return;
    display__draw_rect(x, y, DIALOG__SCROLLBAR_W, h, track_color);
    int thumb_h = h * visible / total;
    if (thumb_h < 6) thumb_h = 6;
    if (thumb_h > h) thumb_h = h;
    int max_scroll = total - visible;
    int thumb_y = max_scroll > 0 ? y + (h - thumb_h) * window_start / max_scroll : y;
    display__fill_rect(x, thumb_y, DIALOG__SCROLLBAR_W, thumb_h, thumb_color);
}

/* -------------------------------------------------------------------------- */
/* Choice-list renderer                                                       */
/* -------------------------------------------------------------------------- */

bruce_result_t dialog__gui_choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected, const bruce_dialog_render_params_t *render_params, const dialog__choice_poll_t *poll
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
    uint64_t selected_at = runtime__now();
    uint64_t next_poll_at = selected_at;
    bool selected_label_overflows = false;
    bool wants_periodic_refresh = render_launcher
                                      ? s_window_renderer.draw_status != NULL
                                      : render_params != NULL && render_params->render_callback != NULL;
    bool long_press_enabled = render_params != NULL && render_params->long_press_enabled;
    if (long_press_enabled && render_params->out_long_press != NULL) { *render_params->out_long_press = false; }

    /* Discard whatever's still queued (typically the press that navigated
     * into this screen) so it can't be replayed as an immediate selection on
     * the freshly drawn list before the user has seen it - e.g. a permission
     * prompt silently auto-confirming "Allow" on the stale press that
     * launched the requesting app. */
    (void)input__flush();
    for (;;) {
        uint64_t now = runtime__now();
        if (poll != NULL && now >= next_poll_at) {
            bool complete = false;
            bruce_result_t poll_result = poll->callback(poll->context, &complete);
            if (poll_result != BRUCE_OK) { return poll_result; }
            if (complete) {
                *poll->out_complete = true;
                return BRUCE_OK;
            }
            next_poll_at = runtime__now() + poll->interval_ms;
        }
        if (wants_periodic_refresh && refresh_interval_ms > 0 && now - rendered_at >= refresh_interval_ms) {
            redraw = true;
        }
        if (selected_label_overflows && now - rendered_at >= DIALOG__MARQUEE_STEP_MS) redraw = true;

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
                if (title_on_pri_fill) { display__fill_rect(left, top, viewport_w, title_h, pri); }
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
            selected_label_overflows = false;

            /* A list longer than one page loses a strip on its right edge to
             * a scrollbar; every row's fill and label shrink to make room for
             * it rather than being drawn under it. */
            bool list_scrollable = choice_count > (size_t)items_per_page;
            int list_w = list_scrollable ? content_w - DIALOG__SCROLLBAR_W - DIALOG__SCROLLBAR_GAP : content_w;

            for (int i = first_visible; i <= last_visible; ++i) {
                int y = list_y + (i - first_visible) * row_h;
                if (i == selected) {
                    display__fill_rect(content_left, y, list_w, row_h, text_color);
                    display__set_text_color(background_color);
                } else {
                    display__set_text_color(text_color);
                }
                display__set_text_size(text_size);
                display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
                int label_left = content_left + DIALOG__MARGIN;
                int label_right = content_left + list_w - DIALOG__MARGIN;
                const bruce_icon_t *icon = icon__get(choices[i].icon_name);
                if (icon != NULL) {
                    int icon_size = row_h - 2;
                    display__draw_bitmap_scaled(
                        label_left,
                        y + 1,
                        icon->bits,
                        icon->width,
                        icon->height,
                        icon_size,
                        icon_size,
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
                bool *overflow = i == selected ? &selected_label_overflows : NULL;
                dialog__gui_draw_row_label(
                    choices[i].label,
                    label_left,
                    y + 1,
                    label_right - label_left,
                    text_size,
                    i == selected,
                    now - selected_at,
                    overflow
                );
            }

            if (list_scrollable) {
                dialog__gui_draw_scrollbar(
                    content_left + list_w + DIALOG__SCROLLBAR_GAP, list_y, usable_h, first_visible,
                    items_per_page, (int)choice_count, border, text_color
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

        uint32_t input_timeout_ms = 100;
        if (poll != NULL) {
            now = runtime__now();
            uint64_t until_poll = next_poll_at > now ? next_poll_at - now : 0;
            if (until_poll < input_timeout_ms) { input_timeout_ms = (uint32_t)until_poll; }
        }
        bruce_input_event_t ev;
        bruce_result_t input_result = input__read(&ev, input_timeout_ms);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) { return BRUCE_ERR_CANCELLED; }
        if (input_result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) { continue; }

        int previous_selected = selected;
        switch (ev.code) {
            case BRUCE_INPUT_CODE_UP:
            case BRUCE_INPUT_CODE_PREV:
                /* Wraps past the top to the last item, same as the launcher's
                 * own lists (bruce_launcher__wrap_index()). An empty list has
                 * nothing to wrap to, so it stays put at 0. */
                if (choice_count > 0) { selected = selected > 0 ? selected - 1 : (int)choice_count - 1; }
                break;
            case BRUCE_INPUT_CODE_DOWN:
            case BRUCE_INPUT_CODE_NEXT:
                if (choice_count > 0) { selected = (size_t)selected + 1 < choice_count ? selected + 1 : 0; }
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
                if (long_press_enabled) {
                    bool is_long_press = dialog__gui_wait_long_press(runtime__now());
                    if (render_params->out_long_press != NULL) { *render_params->out_long_press = is_long_press; }
                }
                *out_selected = (size_t)selected;
                return BRUCE_OK;
            case '\r': *out_selected = (size_t)selected; return BRUCE_OK;
            case BRUCE_INPUT_CODE_BACK: return BRUCE_ERR_CANCELLED;
            default: break;
        }
        redraw = selected != previous_selected;
        if (redraw) selected_at = runtime__now();
    }
}
