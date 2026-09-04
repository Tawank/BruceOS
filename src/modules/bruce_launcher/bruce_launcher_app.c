#include "bruce_launcher_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bruce_launcher_config.h"
#include "bruce_launcher_icons.h"
#include "bruce_launcher_menu.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/status_icon.h"

/* MainMenu visual-style constants. */
#define BRUCE_LAUNCHER_BORDER_PAD 5
#define BRUCE_LAUNCHER_STATUS_H 25
#define BRUCE_LAUNCHER_FONT_SMALL 1
#define BRUCE_LAUNCHER_FONT_MEDIUM 2
#define BRUCE_LAUNCHER_FONT_ADVANCE 6
#define BRUCE_LAUNCHER_PROCESSES_APP "__processes"
#define BRUCE_LAUNCHER_SLIDE_DURATION_MS 160
#define BRUCE_LAUNCHER_EASING_SCALE 1000
#define BRUCE_LAUNCHER_STATUS_REFRESH_MS 1000
#define BRUCE_LAUNCHER_STATUS_TEXT_Y 11
#define BRUCE_LAUNCHER_BACKGROUND_WAIT_MS 1000

/* Theme colors cached from bruce.conf. */
typedef struct {
    uint16_t pri;
    uint16_t sec;
    uint16_t bg;
    uint16_t text_muted;
} bruce_launcher_theme_t;

/* -------------------------------------------------------------------------- */
/* Visual style                                                               */
/* -------------------------------------------------------------------------- */

static void bruce_launcher__get_theme(bruce_launcher_theme_t *theme) {
    theme->pri = config__get_color_primary();
    theme->sec = config__get_color_secondary();
    theme->bg = config__get_color_background();
    theme->text_muted = config__get_color_text_muted();
}

static int bruce_launcher__submenu_font_size(void) {
    return display__width() >= 200 ? BRUCE_LAUNCHER_FONT_MEDIUM : BRUCE_LAUNCHER_FONT_SMALL;
}

static uint32_t bruce_launcher__draw_status_bar(const bruce_launcher_theme_t *theme) {
    size_t count = 0;
    uint32_t revision = 0;
    if (status_icon__list(NULL, 0, &count, &revision) != BRUCE_OK) { return revision; }
    int w = display__width();
    display__fill_rect(
        BRUCE_LAUNCHER_BORDER_PAD + 1,
        6,
        w - 2 * BRUCE_LAUNCHER_BORDER_PAD - 2,
        BRUCE_LAUNCHER_STATUS_H - 7,
        theme->bg
    );
    display__set_text_color(theme->pri);
    display__set_text_bg_color(theme->bg);
    display__set_text_size(BRUCE_LAUNCHER_FONT_SMALL);

    char clock_text[13] = "--:--:--";
    bruce_clock_datetime_t time;
    if (clock__get_local(&time) == BRUCE_OK) {
        bool clock24 = config__get_time_clock24hr();
        int hour = time.hour;
        if (!clock24) {
            hour %= 12;
            if (hour == 0) hour = 12;
        }
        snprintf(clock_text, sizeof(clock_text), "%02d:%02d:%02d", hour, time.minute, time.second);
    }
    int clock_x = BRUCE_LAUNCHER_BORDER_PAD + 8;
    display__set_cursor(clock_x, BRUCE_LAUNCHER_STATUS_TEXT_Y);
    display__print(clock_text);
    int left_limit = clock_x + (int)strlen(clock_text) * BRUCE_LAUNCHER_FONT_ADVANCE + 3;

    int x = w - BRUCE_LAUNCHER_BORDER_PAD - 8;
    int battery = device__get_battery();
    if (battery >= 0) {
        int icon_x = x - 14;
        int icon_y = 11;
        display__draw_rect(icon_x, icon_y, 11, 9, theme->pri);
        display__fill_rect(icon_x + 11, icon_y + 2, 3, 5, theme->pri);
        int fill_width = battery * 7 / 100;
        if (battery > 0 && fill_width == 0) fill_width = 1;
        if (fill_width > 0) { display__fill_rect(icon_x + 2, icon_y + 2, fill_width, 5, theme->pri); }

        char battery_text[12];
        snprintf(battery_text, sizeof(battery_text), "%d%%", battery);
        int text_width = (int)strlen(battery_text) * BRUCE_LAUNCHER_FONT_ADVANCE;
        int text_x = icon_x - text_width - 3;
        display__set_cursor(text_x, BRUCE_LAUNCHER_STATUS_TEXT_Y);
        display__print(battery_text);
        x = text_x - 3;
    }

    for (size_t i = count; i > 0; --i) {
        bruce_status_icon_t icon_value;
        if (status_icon__get(i - 1, &icon_value, NULL) != BRUCE_OK) continue;
        const bruce_status_icon_t *icon = &icon_value;
        x -= icon->width;
        if (x < left_limit) break;
        display__draw_bitmap(
            (int16_t)x,
            (int16_t)(7 + (16 - icon->height) / 2),
            icon->bitmap,
            icon->width,
            icon->height,
            theme->pri
        );
        x -= 3;
    }
    return revision;
}

/* Draw the main border and horizontal status-line separator. */
static void bruce_launcher__draw_main_border(const bruce_launcher_theme_t *theme) {
    int w = display__width();
    int h = display__height();

    display__fill_screen(theme->bg);
    display__draw_round_rect(
        BRUCE_LAUNCHER_BORDER_PAD,
        BRUCE_LAUNCHER_BORDER_PAD,
        w - 2 * BRUCE_LAUNCHER_BORDER_PAD,
        h - 2 * BRUCE_LAUNCHER_BORDER_PAD,
        5,
        theme->pri
    );
    display__draw_line(
        BRUCE_LAUNCHER_BORDER_PAD,
        BRUCE_LAUNCHER_STATUS_H,
        w - BRUCE_LAUNCHER_BORDER_PAD,
        BRUCE_LAUNCHER_STATUS_H,
        theme->pri
    );
}

/* Window-chrome renderer registered with Core's dialog layer (see
 * dialog__set_window_renderer()), so any foreground GUI process can request
 * the same bordered look via dialog__choice()'s window_chrome flag. */
static void bruce_launcher__window_draw_border(void *context) {
    (void)context;
    bruce_launcher_theme_t theme;
    bruce_launcher__get_theme(&theme);
    bruce_launcher__draw_main_border(&theme);
}

/* The choice array of the launcher submenu dialog currently on screen, or
 * {0} when none is. dialog__choice() re-reads choices[i].label on every frame
 * it draws, so re-resolving the labels here -- from the status redraw it
 * already runs on a timer -- is what keeps a toggle entry ("Connect WiFi" vs
 * "Disconnect WiFi") honest while the user sits in the menu watching the
 * BG=1 command behind it finish. Set only for the duration of the
 * dialog__choice() call that owns it; see bruce_launcher__run_gui_menu(). */
static struct {
    bruce_dialog_choice_t *choices;
    const bruce_launcher_entry_t *entries;
    int count;
} s_live_choices;

static void bruce_launcher__refresh_live_choices(void) {
    for (int i = 0; i < s_live_choices.count; ++i) {
        s_live_choices.choices[i].label = bruce_launcher__entry_label(&s_live_choices.entries[i]);
        s_live_choices.choices[i].value = s_live_choices.choices[i].label;
        s_live_choices.choices[i].icon_name = s_live_choices.entries[i].icon_name;
        s_live_choices.choices[i].right_text = NULL;
    }
}

static void bruce_launcher__window_draw_status(void *context) {
    (void)context;
    bruce_launcher__refresh_live_choices();
    bruce_launcher_theme_t theme;
    bruce_launcher__get_theme(&theme);
    (void)bruce_launcher__draw_status_bar(&theme);
}

static void bruce_launcher__draw_centered_text(
    const char *text, int y, int font_size, const bruce_launcher_theme_t *theme
) {
    int text_w = (int)strlen(text) * BRUCE_LAUNCHER_FONT_ADVANCE * font_size;
    display__set_text_size(font_size);
    display__set_text_color(theme->pri);
    display__set_text_bg_color(theme->bg);
    display__set_cursor((display__width() - text_w) / 2, y);
    display__print(text);
}

/* Truncates `text` into `buffer` (capacity `buffer_size`) to fit `max_width`
 * px at `font_size` (a single trailing "." stands in for an ellipsis --
 * there's rarely room for more on the narrow strips this is used for).
 * Returns the pixel width of the truncated text, or 0 if `text` is empty or
 * the budget is too small for even one character (buffer left untouched). */
static int bruce_launcher__truncate_label(
    const char *text, int font_size, int max_width, char *buffer, size_t buffer_size
) {
    if (text == NULL || text[0] == '\0' || max_width <= 0) return 0;
    int advance = BRUCE_LAUNCHER_FONT_ADVANCE * font_size;
    int max_chars = advance > 0 ? max_width / advance : 0;
    if (max_chars <= 0) return 0;

    size_t len = strlen(text);
    if ((int)len > max_chars) {
        int keep = max_chars > 1 ? max_chars - 1 : max_chars;
        if (keep >= (int)buffer_size) keep = (int)buffer_size - 1;
        memcpy(buffer, text, (size_t)keep);
        buffer[keep] = '\0';
        if (max_chars > 1) strncat(buffer, ".", buffer_size - strlen(buffer) - 1);
    } else {
        if (len >= buffer_size) len = buffer_size - 1;
        memcpy(buffer, text, len);
        buffer[len] = '\0';
    }
    return (int)strlen(buffer) * advance;
}

/* Truncates `text` to fit `max_width` px at `font_size` and draws it
 * centered at (cx, y). No-ops on an empty string or a too-small budget. */
static void bruce_launcher__draw_label_fit(
    const char *text, int cx, int y, int font_size, int max_width, uint16_t color, uint16_t bg
) {
    char buffer[40];
    int text_w = bruce_launcher__truncate_label(text, font_size, max_width, buffer, sizeof(buffer));
    if (text_w <= 0) return;
    display__set_text_size(font_size);
    display__set_text_color(color);
    display__set_text_bg_color(bg);
    display__set_cursor((int16_t)(cx - text_w / 2), (int16_t)y);
    display__print(buffer);
}

/* Truncates `text` to fit `max_width` px at `font_size` and draws it
 * left-aligned starting at x, vertically centered on `cy`. Used by the
 * vertical list layout, where every row's label starts at the same left
 * edge instead of being centered under an icon. */
static void bruce_launcher__draw_label_left(
    const char *text, int x, int cy, int font_size, int max_width, uint16_t color, uint16_t bg
) {
    char buffer[40];
    int text_w = bruce_launcher__truncate_label(text, font_size, max_width, buffer, sizeof(buffer));
    if (text_w <= 0) return;
    display__set_text_size(font_size);
    display__set_text_color(color);
    display__set_text_bg_color(bg);
    display__set_cursor((int16_t)x, (int16_t)(cy - 4 * font_size));
    display__print(buffer);
}

/* Thin vertical scrollbar: a track from (x, y) down `h` px, with a thumb
 * sized to the visible fraction of `total` rows/cells and positioned at
 * `window_start`'s fraction of the scrollable range. No-ops when everything
 * already fits (total <= visible). Shared by the grid and list layouts, the
 * two layouts that page through more entries than fit on screen at once. */
static void bruce_launcher__draw_scrollbar(
    int x, int y, int h, int window_start, int visible, int total, const bruce_launcher_theme_t *theme
) {
    if (total <= visible || h <= 4 || visible <= 0) return;
    display__fill_rect((int16_t)x, (int16_t)y, 3, (int16_t)h, theme->bg);
    display__draw_rect((int16_t)x, (int16_t)y, 3, (int16_t)h, theme->sec);
    int thumb_h = h * visible / total;
    if (thumb_h < 6) thumb_h = 6;
    if (thumb_h > h) thumb_h = h;
    int max_scroll = total - visible;
    int thumb_y = max_scroll > 0 ? y + (h - thumb_h) * window_start / max_scroll : y;
    display__fill_rect((int16_t)x, (int16_t)thumb_y, 3, (int16_t)thumb_h, theme->pri);
}

static void bruce_launcher__carousel_icon_sizes(int *out_large, int *out_small) {
    bool narrow = display__width() < 180;
    *out_large = narrow ? 52 : 64;
    *out_small = narrow ? 28 : 36;
}

/* Shared layout math for the horizontal carousel: where the focused icon
 * sits, how far its neighbors are offset along the axis of motion, and
 * where the label line goes. Used by both the static draw (below) and the
 * sliding transition, so a resting frame drawn by either one lines up
 * pixel-for-pixel with the other. (The vertical layout is a plain scrolling
 * list -- see bruce_launcher__draw_root_menu_list -- and needs none of
 * this.) */
static void bruce_launcher__carousel_geometry_h(
    int large, int *out_cx, int *out_cy, int *out_spacing_x, int *out_label_y
) {
    int w = display__width();
    int h = display__height();
    int content_h = h - BRUCE_LAUNCHER_STATUS_H;
    *out_cx = w / 2;
    *out_cy = BRUCE_LAUNCHER_STATUS_H + content_h * 2 / 5;
    *out_spacing_x = w / 2 - w / 7;
    *out_label_y = *out_cy + large / 2 + 6;
}

static void bruce_launcher__draw_root_menu_carousel(
    const bruce_launcher_menu_t *menu, int selected, const bruce_launcher_theme_t *theme,
    const bruce_launcher_render_config_t *render
) {
    int large, small;
    bruce_launcher__carousel_icon_sizes(&large, &small);
    int cx, cy, spacing_x, label_y;
    bruce_launcher__carousel_geometry_h(large, &cx, &cy, &spacing_x, &label_y);

    int previous = (selected + menu->entry_count - 1) % menu->entry_count;
    int next = (selected + 1) % menu->entry_count;
    const bruce_launcher_entry_t *entries = bruce_launcher__menu_entries(menu);

    if (menu->entry_count > 1) {
        bruce_launcher__draw_entry_icon(&entries[previous], cx - spacing_x, cy, small, theme->sec);
        bruce_launcher__draw_entry_icon(&entries[next], cx + spacing_x, cy, small, theme->sec);
    }
    bruce_launcher__draw_entry_icon(&entries[selected], cx, cy, large, theme->pri);

    if (render->carousel_labels != BRUCE_LAUNCHER_LABELS_NONE) {
        bruce_launcher__draw_centered_text(
            bruce_launcher__entry_label(&entries[selected]), label_y, BRUCE_LAUNCHER_FONT_MEDIUM, theme
        );
    }
    if (render->carousel_labels == BRUCE_LAUNCHER_LABELS_ALL && menu->entry_count > 1) {
        int w = display__width();
        int screen_budget = w / 7;
        int half_budget = spacing_x < screen_budget ? spacing_x : screen_budget;
        int side_max_width = 2 * half_budget - 8;
        bruce_launcher__draw_label_fit(
            bruce_launcher__entry_label(&entries[previous]), cx - spacing_x, cy + small / 2 + 3,
            BRUCE_LAUNCHER_FONT_SMALL, side_max_width, theme->sec, theme->bg
        );
        bruce_launcher__draw_label_fit(
            bruce_launcher__entry_label(&entries[next]), cx + spacing_x, cy + small / 2 + 3,
            BRUCE_LAUNCHER_FONT_SMALL, side_max_width, theme->sec, theme->bg
        );
    }
}

/* Vertical layout: a plain scrolling list, one row per entry -- icon on the
 * left, label filling the rest of the row to the right, selected row picked
 * out with a filled card behind both. Every row is the same size; nothing
 * grows, shrinks, or slides, so rows can never overlap each other or their
 * own label the way the old stacked-icon carousel could. When more entries
 * exist than fit on screen, the window scrolls to keep the selection
 * visible and a scrollbar on the right edge shows where in the list that
 * window sits. */
static void bruce_launcher__draw_root_menu_list(
    const bruce_launcher_menu_t *menu, int selected, const bruce_launcher_theme_t *theme,
    const bruce_launcher_render_config_t *render
) {
    int w = display__width();
    int h = display__height();
    int top = BRUCE_LAUNCHER_STATUS_H + 4;
    int content_h = h - BRUCE_LAUNCHER_BORDER_PAD - 2 - top;
    int count = menu->entry_count;

    int icon_size = display__width() < 180 ? 24 : 32;
    int row_h = icon_size + 14;
    if (row_h > content_h) row_h = content_h;
    int visible = row_h > 0 ? content_h / row_h : 1;
    if (visible < 1) visible = 1;
    if (visible > count) visible = count;
    bool scrollable = count > visible;

    int window_start = 0;
    if (scrollable) {
        window_start = selected - visible / 2;
        if (window_start < 0) window_start = 0;
        if (window_start > count - visible) window_start = count - visible;
    }

    int list_h = visible * row_h;
    int list_top = top + (content_h - list_h) / 2;
    int scrollbar_w = scrollable ? 10 : 0;

    int icon_x = BRUCE_LAUNCHER_BORDER_PAD + 8 + icon_size / 2;
    int text_x = BRUCE_LAUNCHER_BORDER_PAD + 8 + icon_size + 10;
    int text_max_w = w - BRUCE_LAUNCHER_BORDER_PAD - 6 - scrollbar_w - text_x;

    const bruce_launcher_entry_t *entries = bruce_launcher__menu_entries(menu);
    for (int i = 0; i < visible; ++i) {
        int index = window_start + i;
        if (index >= count) break;
        bool is_selected = index == selected;
        int row_y = list_top + i * row_h;
        int row_cy = row_y + row_h / 2;

        if (is_selected) {
            display__fill_round_rect(
                (int16_t)(BRUCE_LAUNCHER_BORDER_PAD + 3), (int16_t)(row_y + 1),
                (int16_t)(w - 2 * BRUCE_LAUNCHER_BORDER_PAD - 6 - scrollbar_w), (int16_t)(row_h - 2), 4,
                theme->sec
            );
        }
        uint16_t fg = is_selected ? theme->bg : theme->pri;
        bruce_launcher__draw_entry_icon(&entries[index], icon_x, row_cy, icon_size, fg);
        if (render->carousel_labels != BRUCE_LAUNCHER_LABELS_NONE) {
            bruce_launcher__draw_label_left(
                bruce_launcher__entry_label(&entries[index]), text_x, row_cy, BRUCE_LAUNCHER_FONT_MEDIUM,
                text_max_w, fg, is_selected ? theme->sec : theme->bg
            );
        }
    }

    if (scrollable) {
        bruce_launcher__draw_scrollbar(
            w - BRUCE_LAUNCHER_BORDER_PAD - 6, top, content_h, window_start, visible, count, theme
        );
    }
}

/* Grid page dimensions: the configured column count (clamped to the entry
 * count) if set, otherwise a default of 4 -- wide enough to use the screen
 * without cramming cells. Row count is picked from display height so each
 * cell stays a comfortable, consistent size (2 rows on a short screen, 3 on
 * a taller one) instead of shrinking to force every entry onto one screen;
 * an entry_count too big for one page scrolls instead (see
 * bruce_launcher__draw_root_menu_grid). */
static void bruce_launcher__grid_dimensions(int entry_count, int configured, int *out_cols, int *out_rows) {
    int cols = configured > 0 ? configured : 4;
    if (cols > entry_count) cols = entry_count;
    if (cols < 1) cols = 1;

    int rows = display__height() >= 220 ? 3 : 2;
    int needed_rows = (entry_count + cols - 1) / cols;
    if (rows > needed_rows) rows = needed_rows;
    if (rows < 1) rows = 1;

    *out_cols = cols;
    *out_rows = rows;
}

static void bruce_launcher__draw_root_menu_grid(
    const bruce_launcher_menu_t *menu, int selected, const bruce_launcher_theme_t *theme,
    const bruce_launcher_render_config_t *render
) {
    int w = display__width();
    int h = display__height();
    int top = BRUCE_LAUNCHER_STATUS_H + 4;
    int count = menu->entry_count;
    int cols, rows;
    bruce_launcher__grid_dimensions(count, render->grid_columns, &cols, &rows);
    int total_rows = (count + cols - 1) / cols;
    bool scrollable = total_rows > rows;

    int content_h = h - BRUCE_LAUNCHER_BORDER_PAD - 2 - top;
    int scrollbar_w = scrollable ? 10 : 0;
    int cell_w = (w - 2 * BRUCE_LAUNCHER_BORDER_PAD - 2 - scrollbar_w) / cols;
    int cell_h = content_h / (rows > 0 ? rows : 1);

    int selected_row = selected / cols;
    int window_start_row = 0;
    if (scrollable) {
        window_start_row = selected_row - rows / 2;
        if (window_start_row < 0) window_start_row = 0;
        if (window_start_row > total_rows - rows) window_start_row = total_rows - rows;
    }

    int icon_size = cell_w < cell_h ? cell_w : cell_h;
    icon_size = icon_size * 2 / 5;
    if (icon_size > 40) icon_size = 40;
    if (icon_size < 16) icon_size = 16;

    const bruce_launcher_entry_t *entries = bruce_launcher__menu_entries(menu);
    for (int r = 0; r < rows; ++r) {
        int row_index = window_start_row + r;
        for (int col = 0; col < cols; ++col) {
            int i = row_index * cols + col;
            if (i >= count) continue;
            int cell_x = BRUCE_LAUNCHER_BORDER_PAD + 2 + col * cell_w;
            int cell_y = top + r * cell_h;
            int cx = cell_x + cell_w / 2;
            bool is_selected = i == selected;

            /* The selection card covers the whole cell -- icon and label
             * together -- so "selected" visibly includes the text, not just
             * a box drawn around the icon above it. */
            if (is_selected) {
                display__fill_round_rect(
                    (int16_t)(cell_x + 2), (int16_t)(cell_y + 2), (int16_t)(cell_w - 4),
                    (int16_t)(cell_h - 4), 5, theme->sec
                );
            }
            uint16_t fg = is_selected ? theme->bg : theme->sec;
            int icon_y = cell_y + (render->grid_labels ? cell_h * 2 / 5 : cell_h / 2);
            bruce_launcher__draw_entry_icon(&entries[i], cx, icon_y, icon_size, fg);

            if (render->grid_labels) {
                bruce_launcher__draw_label_fit(
                    bruce_launcher__entry_label(&entries[i]), cx, icon_y + icon_size / 2 + 4,
                    BRUCE_LAUNCHER_FONT_SMALL, cell_w - 8, fg, is_selected ? theme->sec : theme->bg
                );
            }
        }
    }

    if (scrollable) {
        bruce_launcher__draw_scrollbar(
            w - BRUCE_LAUNCHER_BORDER_PAD - 6, top, content_h, window_start_row, rows, total_rows, theme
        );
    }

    if (!render->grid_labels && count > 0) {
        bruce_launcher__draw_centered_text(
            bruce_launcher__entry_label(&entries[selected]), h - BRUCE_LAUNCHER_BORDER_PAD - 14,
            BRUCE_LAUNCHER_FONT_SMALL, theme
        );
    }
}

/* Root-menu entry point: dispatches to whichever layout render.layout
 * selects (see bruce_launcher_config.h). Nested submenus never call this --
 * they always use Core's plain list dialog (dialog__choice_launcher),
 * regardless of the root's layout. */
static void bruce_launcher__draw_root_menu(
    const bruce_launcher_menu_t *menu, int selected, const bruce_launcher_theme_t *theme,
    const bruce_launcher_render_config_t *render
) {
    int w = display__width();
    int h = display__height();
    if (menu->entry_count == 0) {
        display__set_text_size(BRUCE_LAUNCHER_FONT_SMALL);
        display__set_text_color(theme->text_muted);
        display__set_text_bg_color(theme->bg);
        const char *empty_label = "No entries";
        int text_w = (int)strlen(empty_label) * BRUCE_LAUNCHER_FONT_ADVANCE * BRUCE_LAUNCHER_FONT_SMALL;
        display__set_cursor((w - text_w) / 2, (h + BRUCE_LAUNCHER_STATUS_H) / 2);
        display__print(empty_label);
        return;
    }

    switch (render->layout) {
        case BRUCE_LAUNCHER_LAYOUT_GRID:
            bruce_launcher__draw_root_menu_grid(menu, selected, theme, render);
            return;
        case BRUCE_LAUNCHER_LAYOUT_CAROUSEL_V:
            bruce_launcher__draw_root_menu_list(menu, selected, theme, render);
            return;
        default: bruce_launcher__draw_root_menu_carousel(menu, selected, theme, render); return;
    }
}

static uint16_t bruce_launcher__blend_color(uint16_t from, uint16_t to, int amount) {
    int inverse = BRUCE_LAUNCHER_EASING_SCALE - amount;
    int r = (((from >> 11) & 0x1f) * inverse + ((to >> 11) & 0x1f) * amount) / BRUCE_LAUNCHER_EASING_SCALE;
    int g = (((from >> 5) & 0x3f) * inverse + ((to >> 5) & 0x3f) * amount) / BRUCE_LAUNCHER_EASING_SCALE;
    int b = ((from & 0x1f) * inverse + (to & 0x1f) * amount) / BRUCE_LAUNCHER_EASING_SCALE;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static int bruce_launcher__wrap_index(int index, int count) {
    index %= count;
    return index < 0 ? index + count : index;
}

/* center_x/center_y is where the focused (relative==0) icon sits;
 * spacing_x is how far one full step moves along the horizontal axis of
 * motion. When `label_width` is > 0 (LABELS_ALL), an icon that is at least
 * halfway to a side slot -- the same halfway point that decides z-order and
 * which entry owns the big centered label below -- also gets its own small
 * label drawn at its live position. That single threshold is what keeps the
 * side labels sliding continuously in step with their icons instead of
 * popping in only once the slide finishes: whichever icon is fading out of
 * "focused" past the midpoint is already fading its label in as a side
 * label, and vice versa for the icon fading into focus. */
static void bruce_launcher__draw_sliding_icon(
    const bruce_launcher_menu_t *menu, int from, int relative, int direction, int progress, int center_x,
    int center_y, int spacing_x, int small, int large, int label_width, const bruce_launcher_theme_t *theme
) {
    int position = relative * BRUCE_LAUNCHER_EASING_SCALE - direction * progress;
    int distance = abs(position);
    if (distance > BRUCE_LAUNCHER_EASING_SCALE) { distance = BRUCE_LAUNCHER_EASING_SCALE; }
    int size = large - (large - small) * distance / BRUCE_LAUNCHER_EASING_SCALE;
    uint16_t color = bruce_launcher__blend_color(theme->pri, theme->sec, distance);
    int x = center_x + position * spacing_x / BRUCE_LAUNCHER_EASING_SCALE;
    int entry = bruce_launcher__wrap_index(from + relative, menu->entry_count);
    const bruce_launcher_entry_t *drawn = &bruce_launcher__menu_entries(menu)[entry];
    bruce_launcher__draw_entry_icon(drawn, x, center_y, size, color);

    if (label_width > 0 && 2 * distance >= BRUCE_LAUNCHER_EASING_SCALE) {
        bruce_launcher__draw_label_fit(
            bruce_launcher__entry_label(drawn), x, center_y + size / 2 + 3, BRUCE_LAUNCHER_FONT_SMALL,
            label_width, color, theme->bg
        );
    }
}

static void bruce_launcher__draw_root_transition(
    const bruce_launcher_menu_t *menu, int from, int direction, int progress,
    const bruce_launcher_theme_t *theme, const bruce_launcher_render_config_t *render
) {
    int large, small;
    bruce_launcher__carousel_icon_sizes(&large, &small);
    int cx, cy, spacing_x, label_y;
    bruce_launcher__carousel_geometry_h(large, &cx, &cy, &spacing_x, &label_y);
    int outer_before = direction > 0 ? -1 : 1;
    int outer_after = direction > 0 ? 2 : -2;
    int destination = direction;

    int label_width = 0;
    if (render->carousel_labels == BRUCE_LAUNCHER_LABELS_ALL && menu->entry_count > 1) {
        int w = display__width();
        int screen_budget = w / 7;
        int half_budget = spacing_x < screen_budget ? spacing_x : screen_budget;
        label_width = 2 * half_budget - 8;
    }

    bruce_launcher__draw_sliding_icon(
        menu, from, outer_before, direction, progress, cx, cy, spacing_x, small, large, label_width, theme
    );
    bruce_launcher__draw_sliding_icon(
        menu, from, outer_after, direction, progress, cx, cy, spacing_x, small, large, label_width, theme
    );
    if (progress < BRUCE_LAUNCHER_EASING_SCALE / 2) {
        bruce_launcher__draw_sliding_icon(
            menu, from, destination, direction, progress, cx, cy, spacing_x, small, large, label_width, theme
        );
        bruce_launcher__draw_sliding_icon(
            menu, from, 0, direction, progress, cx, cy, spacing_x, small, large, label_width, theme
        );
    } else {
        bruce_launcher__draw_sliding_icon(
            menu, from, 0, direction, progress, cx, cy, spacing_x, small, large, label_width, theme
        );
        bruce_launcher__draw_sliding_icon(
            menu, from, destination, direction, progress, cx, cy, spacing_x, small, large, label_width, theme
        );
    }

    if (render->carousel_labels != BRUCE_LAUNCHER_LABELS_NONE) {
        int label_entry = progress < BRUCE_LAUNCHER_EASING_SCALE / 2
                              ? from
                              : bruce_launcher__wrap_index(from + direction, menu->entry_count);
        bruce_launcher__draw_centered_text(
            bruce_launcher__entry_label(&bruce_launcher__menu_entries(menu)[label_entry]), label_y,
            BRUCE_LAUNCHER_FONT_MEDIUM, theme
        );
    }
}

static int bruce_launcher__navigation_direction(int32_t code) {
    switch (code) {
        case BRUCE_INPUT_CODE_LEFT:
        case BRUCE_INPUT_CODE_UP:
        case BRUCE_INPUT_CODE_PREV: return -1;
        case BRUCE_INPUT_CODE_RIGHT:
        case BRUCE_INPUT_CODE_DOWN:
        case BRUCE_INPUT_CODE_NEXT: return 1;
        default: return 0;
    }
}

/* Slides the horizontal carousel from `from` one step in `direction` (-1 or
 * +1). Repeated navigation presses replace the in-flight transition with one
 * from the newest selection, so rapid input cannot queue a full animation per
 * press. The last frame (linear == EASING_SCALE) is drawn with the plain
 * static renderer instead of the transition one, at the settled index, so
 * it comes out pixel-identical to what a plain redraw of that selection
 * would show. (Only the horizontal carousel animates; the grid redraws
 * instantly and the vertical list has no icons to slide.) */
static bruce_result_t bruce_launcher__animate_root_menu(
    const bruce_launcher_menu_t *menu, int from, int direction, int *selected, const bruce_launcher_theme_t *theme,
    const bruce_launcher_render_config_t *render, uint32_t *icon_revision
) {
    uint64_t started_at = runtime__now();
    for (;;) {
        uint64_t elapsed = runtime__now() - started_at;
        int linear = elapsed >= BRUCE_LAUNCHER_SLIDE_DURATION_MS
                         ? BRUCE_LAUNCHER_EASING_SCALE
                         : (int)(elapsed * BRUCE_LAUNCHER_EASING_SCALE / BRUCE_LAUNCHER_SLIDE_DURATION_MS);
        int progress = linear * linear * (3 * BRUCE_LAUNCHER_EASING_SCALE - 2 * linear) /
                       (BRUCE_LAUNCHER_EASING_SCALE * BRUCE_LAUNCHER_EASING_SCALE);

        bruce_result_t frame;
        do {
            frame = display__begin_frame();
            if (frame == BRUCE_ERR_NOT_FOREGROUND) { runtime__sleep(BRUCE_LAUNCHER_BACKGROUND_WAIT_MS); }
        } while (frame == BRUCE_ERR_NOT_FOREGROUND);
        if (frame != BRUCE_OK) { return frame; }

        bruce_launcher__draw_main_border(theme);
        if (linear == BRUCE_LAUNCHER_EASING_SCALE) {
            int settled = bruce_launcher__wrap_index(from + direction, menu->entry_count);
            bruce_launcher__draw_root_menu_carousel(menu, settled, theme, render);
        } else {
            bruce_launcher__draw_root_transition(menu, from, direction, progress, theme, render);
        }
        *icon_revision = bruce_launcher__draw_status_bar(theme);
        frame = display__present();
        if (frame != BRUCE_OK) { return frame; }
        if (linear == BRUCE_LAUNCHER_EASING_SCALE) { return BRUCE_OK; }

        bruce_input_event_t event;
        while (input__peek(&event) == BRUCE_OK) {
            if (event.action != BRUCE_INPUT_PRESS) {
                (void)input__read(&event, 0);
                continue;
            }
            int next_direction = bruce_launcher__navigation_direction(event.code);
            if (next_direction == 0) { break; }
            (void)input__read(&event, 0);
            from = *selected;
            direction = next_direction;
            *selected = bruce_launcher__wrap_index(*selected + direction, menu->entry_count);
            started_at = runtime__now();
        }
    }
}

static size_t bruce_launcher__process_candidates(bruce_process_snapshot_t *processes, size_t capacity) {
    size_t count = 0;
    size_t written = 0;
    bruce_process_id_t self = process__current_id();
    if (process__list(processes, capacity, &count) != BRUCE_OK) { return 0; }
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].id != self && processes[i].presentable && !processes[i].blocked_on_wait &&
            processes[i].state == BRUCE_PROCESS_BACKGROUND) {
            if (written != i) processes[written] = processes[i];
            written++;
        }
    }
    return written;
}

static void bruce_launcher__process_layout(
    const bruce_process_snapshot_t *processes, size_t count, int selected, bruce_display_tile_t *tiles,
    const bruce_launcher_theme_t *theme
) {
    int w = display__width();
    int h = display__height();
    int top = BRUCE_LAUNCHER_STATUS_H + 2;
    int cols = (count == 1 || (count == 2 && h > w)) ? 1 : 2;
    int rows = (int)((count + (size_t)cols - 1) / (size_t)cols);
    int cell_w = w / cols;
    int cell_h = (h - top) / rows;

    display__fill_screen(theme->bg);
    display__set_text_size(1);
    display__set_text_color(theme->pri);
    display__set_text_bg_color(theme->bg);
    display__set_cursor(4, 7);
    display__print("Processes  arrows: move  select: open  back: exit");
    display__draw_line(0, BRUCE_LAUNCHER_STATUS_H, w - 1, BRUCE_LAUNCHER_STATUS_H, theme->pri);

    for (size_t i = 0; i < count; ++i) {
        int col = (int)i % cols;
        int row = (int)i / cols;
        int cell_x = col * cell_w;
        int cell_y = top + row * cell_h;
        int right = col == cols - 1 ? w : cell_x + cell_w;
        int bottom = row == rows - 1 ? h : cell_y + cell_h;
        tiles[i].process_id = processes[i].id;
        tiles[i].rect = (bruce_display_rect_t){
            .x = cell_x + 3,
            .y = cell_y + 12,
            .width = right - cell_x - 6,
            .height = bottom - cell_y - 15,
        };
        display__set_cursor(cell_x + 4, cell_y + 2);
        display__print(processes[i].name);
        display__draw_rect(
            cell_x + 1,
            cell_y + 10,
            right - cell_x - 2,
            bottom - cell_y - 11,
            (int)i == selected ? theme->pri : theme->sec
        );
    }
}

static bruce_result_t bruce_launcher__draw_process_page(
    const bruce_process_snapshot_t *processes, size_t count, int selected, const bruce_launcher_theme_t *theme
) {
    bruce_result_t result = display__set_tiles(NULL, 0);
    if (result != BRUCE_OK) { return result; }
    result = display__begin_frame();
    if (result != BRUCE_OK) { return result; }
    bruce_display_tile_t tiles[BRUCE_DISPLAY_MAX_TILES];
    bruce_launcher__process_layout(processes, count, selected, tiles, theme);
    result = display__present();
    if (result != BRUCE_OK || count == 0) { return result; }
    return display__set_tiles(tiles, count);
}

static int bruce_launcher__run_process_switcher(const bruce_launcher_theme_t *theme) {
    const size_t candidate_capacity = 16;
    bruce_process_snapshot_t *candidates =
        (bruce_process_snapshot_t *)memory__calloc(candidate_capacity, sizeof(*candidates));
    if (candidates == NULL) return BRUCE_ERR_NO_MEMORY;

    size_t page = 0;
    int selected = 0;
    bool redraw = true;
    for (;;) {
        size_t total = bruce_launcher__process_candidates(candidates, candidate_capacity);
        size_t pages = total == 0 ? 1 : (total + BRUCE_DISPLAY_MAX_TILES - 1) / BRUCE_DISPLAY_MAX_TILES;
        if (page >= pages) page = pages - 1;
        size_t start = page * BRUCE_DISPLAY_MAX_TILES;
        size_t page_count = total > start ? total - start : 0;
        if (page_count > BRUCE_DISPLAY_MAX_TILES) page_count = BRUCE_DISPLAY_MAX_TILES;
        if (selected >= (int)page_count) selected = page_count > 0 ? (int)page_count - 1 : 0;

        if (redraw) {
            bruce_result_t draw =
                bruce_launcher__draw_process_page(&candidates[start], page_count, selected, theme);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            redraw = false;
        }

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            memory__free(candidates);
            return 0;
        }
        if (input_result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) { continue; }
        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B) {
            (void)display__set_tiles(NULL, 0);
            memory__free(candidates);
            return 0;
        }
        if ((event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_PREV ||
             event.code == BRUCE_INPUT_CODE_LEFT) &&
            selected > 0) {
            selected--;
            redraw = true;
        } else if ((event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT ||
                    event.code == BRUCE_INPUT_CODE_RIGHT) &&
                   selected + 1 < (int)page_count) {
            selected++;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_LEFT && page > 0) {
            page--;
            selected = 0;
            redraw = true;
        } else if (event.code == BRUCE_INPUT_CODE_RIGHT && page + 1 < pages) {
            page++;
            selected = 0;
            redraw = true;
        } else if ((event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A) &&
                   page_count > 0) {
            bruce_process_id_t target = candidates[start + (size_t)selected].id;
            bruce_process_snapshot_t snapshot;
            if (process__snapshot(target, &snapshot) == BRUCE_OK) {
                (void)display__set_tiles(NULL, 0);
                (void)process__foreground(target);
                memory__free(candidates);
                return 0;
            }
            redraw = true;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Command dispatch                                                           */
/* -------------------------------------------------------------------------- */

/* True iff the command's leading "key=value" environment tokens (the same ones
 * app_runner__run_command() parses off the front of the line) assign `name`. */
static bool bruce_launcher__command_sets(const char *command, const char *name) {
    size_t name_len = strlen(name);
    const char *cursor = command;
    for (;;) {
        while (*cursor == ' ') cursor++;
        const char *token_end = cursor;
        while (*token_end != '\0' && *token_end != ' ') token_end++;
        size_t token_len = (size_t)(token_end - cursor);
        if (token_len == 0 || memchr(cursor, '=', token_len) == NULL) return false;
        if (token_len > name_len && cursor[name_len] == '=' && strncmp(cursor, name, name_len) == 0) {
            return true;
        }
        cursor = token_end;
    }
}

static int bruce_launcher__run_entry(const bruce_launcher_entry_t *entry) {
    if (strcmp(entry->command, BRUCE_LAUNCHER_PROCESSES_APP) == 0) {
        bruce_launcher_theme_t theme;
        bruce_launcher__get_theme(&theme);
        return bruce_launcher__run_process_switcher(&theme);
    }

    /* Everything launched from the GUI menu is GUI=1 unless the entry opts out
     * explicitly, including "BG=1" entries: those run headless (they never take
     * the foreground, so the launcher stays on screen), but GUI=1 is what makes
     * their notification__push()es render as banners instead of going to a
     * console nobody is looking at. process__foreground_push_locked() -- not
     * gui_requested -- is what makes a process a switch target, so a background
     * worker with no screen still never appears in the process switcher.
     *
     * BG itself is left to app_runner__run_command() to parse off the line; the
     * launcher only picks the default for entries that don't say. */
    const char *command = entry->command;
    char *prefixed_command = NULL;
    if (runtime__gui_requested() && !bruce_launcher__command_sets(entry->command, "GUI")) {
        size_t capacity = strlen(entry->command) + sizeof("GUI=1 ");
        prefixed_command = (char *)memory__malloc(capacity);
        if (prefixed_command == NULL) return BRUCE_ERR_NO_MEMORY;
        snprintf(prefixed_command, capacity, "GUI=1 %s", entry->command);
        command = prefixed_command;
    }
    int result = app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);
    memory__free(prefixed_command);

    if (result < 0) {
        const size_t message_capacity = 128;
        char *message = (char *)memory__malloc(message_capacity);
        if (message != NULL) {
            ext_mem_loader__format_error_message(
                bruce_launcher__entry_label(entry), result, message, message_capacity
            );
            (void)dialog__message(BRUCE_DIALOG_ERROR, "Launch failed", message);
            memory__free(message);
        }
    }
    return result;
}

/* -------------------------------------------------------------------------- */
/* GUI menu runner                                                            */
/* -------------------------------------------------------------------------- */

/* A dialog__choice_launcher() call losing (and this process later regaining)
 * foreground - e.g. the process switcher, or system_menu's overlay taking it
 * while its own dialog is open - surfaces as BRUCE_ERR_CANCELLED same as a
 * genuine Back/Esc (see core/dialog/dialog.c); this tells them apart the same
 * way archive_app.c/filemanager_app.c's identical helper does, so a real
 * handoff redraws the same submenu instead of being treated as the user
 * stepping back a level. */
static bool bruce_launcher__resume_after_handoff(void) {
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

/* The root is a horizontal carousel. Nested menus use Core's choice renderer
 * inside the launcher's status bar and outer border. */
static int bruce_launcher__run_gui_menu(const bruce_launcher_menu_t *menu) {
    bruce_launcher_theme_t theme;
    bruce_launcher__get_theme(&theme);

    if (!menu->is_root) {
        /* Keep submenu navigation on one stack frame. The previous implementation
         * called this function again for every nested submenu, so a deep menu
         * tree exhausted the launcher task stack even though choices were heap
         * allocated. */
        const bruce_launcher_menu_t **parents =
            (const bruce_launcher_menu_t **)memory__calloc(BRUCE_LAUNCHER_MAX_ENTRIES, sizeof(*parents));
        bruce_dialog_choice_t *choices =
            (bruce_dialog_choice_t *)memory__calloc(BRUCE_LAUNCHER_MAX_ENTRIES, sizeof(*choices));
        if (parents == NULL || choices == NULL) {
            memory__free(parents);
            memory__free(choices);
            return BRUCE_ERR_NO_MEMORY;
        }

        (void)input__flush();
        size_t depth = 0;
        const bruce_launcher_menu_t *current = menu;
        for (;;) {
            const bruce_launcher_entry_t *entries = bruce_launcher__menu_entries(current);
            if (display__width() <= 0 || display__height() <= 0) {
                (void)runtime__delay(20);
                continue;
            }

            bruce_result_t frame = display__begin_frame();
            if (frame == BRUCE_ERR_NOT_FOREGROUND) {
                (void)runtime__sleep(BRUCE_LAUNCHER_BACKGROUND_WAIT_MS);
                continue;
            }
            if (frame != BRUCE_OK) {
                memory__free(parents);
                memory__free(choices);
                return frame;
            }
            bruce_launcher__draw_main_border(&theme);
            (void)bruce_launcher__draw_status_bar(&theme);
            frame = display__present();
            if (frame != BRUCE_OK) {
                memory__free(parents);
                memory__free(choices);
                return frame;
            }

            size_t selected = 0;
            /* Published only across the dialog call: bruce_launcher__window_draw_status()
             * runs on other processes' dialogs too, and must not touch this
             * array once it goes out of scope. */
            s_live_choices.choices = choices;
            s_live_choices.entries = entries;
            s_live_choices.count = current->entry_count;
            bruce_launcher__refresh_live_choices();
            bruce_result_t result = dialog__choice_launcher(
                current->title, NULL, choices, (size_t)current->entry_count, &selected
            );
            s_live_choices.count = 0;
            if (result == BRUCE_ERR_CANCELLED) {
                if (bruce_launcher__resume_after_handoff()) {
                    /* Foreground was lost (alt-tab, system_menu, ...) and has
                     * now returned: redraw the same menu instead of stepping
                     * up a level. */
                    (void)input__flush();
                    continue;
                }
                /* Esc/Back steps up one level, same as selecting the "Back"
                 * entry below -- not all the way out of the whole submenu
                 * stack. Only at the top of this stack (depth 0) does it
                 * leave the submenu entirely. */
                if (depth == 0) break;
                current = parents[--depth];
                (void)input__flush();
                continue;
            }
            if (result == BRUCE_ERR_NOT_FOREGROUND) {
                (void)runtime__sleep(BRUCE_LAUNCHER_BACKGROUND_WAIT_MS);
                continue;
            }
            if (result != BRUCE_OK) continue;

            const bruce_launcher_entry_t *entry = &entries[selected];
            if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) {
                if (depth == 0) break;
                current = parents[--depth];
                (void)input__flush();
                continue;
            }
            if (entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU) {
                if (depth >= BRUCE_LAUNCHER_MAX_ENTRIES) continue;
                parents[depth++] = current;
                current = bruce_launcher__entry_submenu(current, entry);
            } else {
                (void)bruce_launcher__run_entry(entry);
            }
            (void)input__flush();
        }
        memory__free(parents);
        memory__free(choices);
        return 0;
    }

    (void)input__flush();

    bruce_launcher_render_config_t render;
    bruce_launcher_config__load(&render);

    int selected = 0;
    int last_drawn = -1;
    int last_width = -1;
    int last_height = -1;
    uint32_t icon_revision = UINT32_MAX;
    uint64_t status_drawn_at = 0;
    for (;;) {
        int width = display__width();
        int height = display__height();
        if (width <= 0 || height <= 0) {
            last_drawn = -1;
            last_width = width;
            last_height = height;
            (void)runtime__delay(20);
            continue;
        }
        if (width != last_width || height != last_height) {
            last_drawn = -1;
            last_width = width;
            last_height = height;
        }

        if (selected != last_drawn) {
            bruce_result_t frame = display__begin_frame();
            if (frame == BRUCE_ERR_NOT_FOREGROUND) {
                (void)runtime__sleep(BRUCE_LAUNCHER_BACKGROUND_WAIT_MS);
                continue;
            }
            if (frame != BRUCE_OK) { return frame; }
            bruce_launcher__draw_main_border(&theme);
            bruce_launcher__draw_root_menu(menu, selected, &theme, &render);
            icon_revision = bruce_launcher__draw_status_bar(&theme);
            status_drawn_at = runtime__now();
            frame = display__present();
            if (frame != BRUCE_OK) { return frame; }
            last_drawn = selected;
        }

        size_t icon_count = 0;
        uint32_t current_revision = 0;
        uint64_t now = runtime__now();
        if ((status_icon__list(NULL, 0, &icon_count, &current_revision) == BRUCE_OK &&
             current_revision != icon_revision) ||
            now - status_drawn_at >= BRUCE_LAUNCHER_STATUS_REFRESH_MS) {
            bruce_result_t frame = display__begin_frame();
            if (frame == BRUCE_OK) {
                icon_revision = bruce_launcher__draw_status_bar(&theme);
                status_drawn_at = now;
                (void)display__present();
            }
        }

        bruce_input_event_t ev;
        bruce_result_t result = input__read(&ev, 100);
        if (result == BRUCE_ERR_NOT_FOREGROUND) {
            /* Core clears this process's display on its first frame after it
             * regains foreground. Invalidate the cached selection so that
             * frame repaints the whole launcher, not only the status bar. */
            last_drawn = -1;
            (void)runtime__sleep(BRUCE_LAUNCHER_BACKGROUND_WAIT_MS);
            continue;
        }
        if (result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) { continue; }

        switch (ev.code) {
            case BRUCE_INPUT_CODE_LEFT:
            case BRUCE_INPUT_CODE_UP:
            case BRUCE_INPUT_CODE_PREV:
            case BRUCE_INPUT_CODE_RIGHT:
            case BRUCE_INPUT_CODE_DOWN:
            case BRUCE_INPUT_CODE_NEXT: {
                if (menu->entry_count == 0) break;

                if (render.layout == BRUCE_LAUNCHER_LAYOUT_GRID) {
                    /* A real 2D layout: LEFT/RIGHT step within the current row
                     * (wrapping at that row's own length, which may be short
                     * on a partially-filled last row), UP/DOWN step by a full
                     * row (wrapping the whole grid). PREV/NEXT -- the generic
                     * two-button-hardware codes -- fall back to flat list
                     * order, same as they would for a plain list dialog. */
                    int count = menu->entry_count;
                    int cols, unused_rows;
                    bruce_launcher__grid_dimensions(count, render.grid_columns, &cols, &unused_rows);
                    (void)unused_rows;
                    switch (ev.code) {
                        case BRUCE_INPUT_CODE_PREV: selected = (selected + count - 1) % count; break;
                        case BRUCE_INPUT_CODE_NEXT: selected = (selected + 1) % count; break;
                        case BRUCE_INPUT_CODE_LEFT:
                        case BRUCE_INPUT_CODE_RIGHT: {
                            int row_start = (selected / cols) * cols;
                            int row_len = count - row_start < cols ? count - row_start : cols;
                            int col = selected - row_start;
                            col = ev.code == BRUCE_INPUT_CODE_LEFT ? (col + row_len - 1) % row_len
                                                                    : (col + 1) % row_len;
                            selected = row_start + col;
                            break;
                        }
                        default: /* UP / DOWN */
                            selected = bruce_launcher__wrap_index(
                                selected + (ev.code == BRUCE_INPUT_CODE_UP ? -cols : cols), count
                            );
                            break;
                    }
                    last_drawn = -1;
                    break;
                }

                /* The vertical list and the horizontal carousel both accept
                 * all six codes -- LEFT/UP and RIGHT/DOWN alike mean
                 * "previous"/"next" -- so hardware with only a left/right
                 * pair still drives the list, and vice versa. */
                bool go_previous = ev.code == BRUCE_INPUT_CODE_LEFT || ev.code == BRUCE_INPUT_CODE_UP ||
                                    ev.code == BRUCE_INPUT_CODE_PREV;
                int direction = go_previous ? -1 : 1;
                int previous = selected;
                selected = bruce_launcher__wrap_index(selected + direction, menu->entry_count);

                /* Only the horizontal carousel has icons worth sliding; the
                 * vertical list just scrolls its window and redraws, same as
                 * the grid does above. */
                if (menu->entry_count > 1 && render.layout == BRUCE_LAUNCHER_LAYOUT_CAROUSEL_H) {
                    result = bruce_launcher__animate_root_menu(
                        menu, previous, direction, &selected, &theme, &render, &icon_revision
                    );
                    if (result != BRUCE_OK) { return result; }
                    last_drawn = selected;
                } else {
                    last_drawn = -1;
                }
                break;
            }
            case BRUCE_INPUT_CODE_SELECT: {
                if (menu->entry_count == 0) { break; }
                const bruce_launcher_entry_t *entry = &bruce_launcher__menu_entries(menu)[selected];
                if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) { return 0; }
                if (entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU) {
                    (void)bruce_launcher__run_gui_menu(bruce_launcher__entry_submenu(menu, entry));
                } else {
                    (void)bruce_launcher__run_entry(entry);
                }
                (void)input__flush();
                last_drawn = -1;
                break;
            }
            case BRUCE_INPUT_CODE_BACK: return 0;
            default: break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Terminal menu runner                                                       */
/* -------------------------------------------------------------------------- */

/* Terminal fallback used when the launcher is started without GUI=1. Keeps
 * the original renderer-agnostic dialog__choice() path so serial/terminal
 * usage and the host selftest continue to work. */
static int bruce_launcher__run_terminal_menu(const bruce_launcher_menu_t *menu) {
    const bruce_launcher_entry_t *entries = bruce_launcher__menu_entries(menu);
    bruce_dialog_choice_t *choices =
        (bruce_dialog_choice_t *)memory__calloc((size_t)menu->capacity + 1, sizeof(*choices));
    if (choices == NULL) { return BRUCE_ERR_NO_MEMORY; }

    for (;;) {
        for (int i = 0; i < menu->entry_count; ++i) {
            choices[i].label = bruce_launcher__entry_label(&entries[i]);
            choices[i].value = choices[i].label;
            choices[i].icon_name = entries[i].icon_name;
            choices[i].right_text = NULL;
        }

        choices[menu->entry_count].label = "Back";
        choices[menu->entry_count].value = choices[menu->entry_count].label;
        choices[menu->entry_count].icon_name = NULL;
        choices[menu->entry_count].right_text = NULL;

        size_t choice = 0;
        bruce_result_t choice_result =
            dialog__choice(menu->title, "Select an app", choices, (size_t)menu->entry_count + 1, &choice);

        if (choice_result == BRUCE_ERR_CANCELLED) { break; }
        if (choice_result != BRUCE_OK) {
            printf("Invalid choice (%d), try again.\n", choice_result);
            continue;
        }

        const bruce_launcher_entry_t *entry = &entries[(int)choice];
        if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) { break; }
        if (entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU) {
            (void)bruce_launcher__run_terminal_menu(bruce_launcher__entry_submenu(menu, entry));
        } else {
            (void)bruce_launcher__run_entry(entry);
        }
    }

    memory__free(choices);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

int bruce_launcher_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Open the Bruce launcher.\nUsage: bruce_launcher [config ...]\n");
        return BRUCE_OK;
    }

    /* "bruce_launcher config ..." reconfigures how the root menu renders
     * (bruce_launcher_config.c) instead of running the menu itself; routed
     * here rather than through a separate registered app so the render
     * settings stay a private implementation detail of this module, the
     * same way the menu tree parser and icon drawing do. */
    if (argc > 1 && strcmp(argv[1], "config") == 0) {
        return bruce_launcher_config__run(argc - 1, argv + 1);
    }

    bruce_launcher_menu_t *root = bruce_launcher__menu_load();
    if (root == NULL) {
        printf("Failed to load launcher configuration\n");
        return BRUCE_ERR_INTERNAL;
    }

    int result;
    if (runtime__gui_requested()) {
        const bruce_dialog_window_renderer_t window_renderer = {
            .padding_top = BRUCE_LAUNCHER_STATUS_H + 1,
            .padding_right = BRUCE_LAUNCHER_BORDER_PAD + 1,
            .padding_bottom = BRUCE_LAUNCHER_BORDER_PAD + 1,
            .padding_left = BRUCE_LAUNCHER_BORDER_PAD + 1,
            .text_size = bruce_launcher__submenu_font_size(),
            .draw_border = bruce_launcher__window_draw_border,
            .draw_status = bruce_launcher__window_draw_status,
            .status_refresh_interval_ms = BRUCE_LAUNCHER_STATUS_REFRESH_MS,
        };
        dialog__set_window_renderer(&window_renderer, NULL);
        result = bruce_launcher__run_gui_menu(root);
        dialog__set_window_renderer(NULL, NULL);
        s_live_choices.count = 0;
    } else {
        result = bruce_launcher__run_terminal_menu(root);
    }

    bruce_launcher__menu_free(root);
    display__fill_screen(BRUCE_COLOR_BLACK);
    return result;
}
