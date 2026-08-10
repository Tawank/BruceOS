#include "bruce_launcher_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bruce_launcher_icons.h"
#include "bruce_launcher_menu.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
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
} bruce_launcher_theme_t;

/* -------------------------------------------------------------------------- */
/* Visual style                                                               */
/* -------------------------------------------------------------------------- */

static void bruce_launcher__get_theme(bruce_launcher_theme_t *theme) {
    theme->pri = config__get_theme_primary();
    theme->sec = config__get_theme_secondary();
    theme->bg = config__get_theme_background();
}

static int bruce_launcher__submenu_font_size(void) {
    return display__width() >= 200 ? BRUCE_LAUNCHER_FONT_MEDIUM : BRUCE_LAUNCHER_FONT_SMALL;
}

static uint32_t bruce_launcher__draw_status_bar(const bruce_launcher_theme_t *theme) {
    bruce_status_icon_t icons[BRUCE_STATUS_ICON_MAX];
    size_t count = 0;
    uint32_t revision = 0;
    if (status_icon__list(icons, BRUCE_STATUS_ICON_MAX, &count, &revision) != BRUCE_OK) { return revision; }
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
        const bruce_status_icon_t *icon = &icons[i - 1];
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

static void bruce_launcher__draw_root_menu(
    const bruce_launcher_menu_t *menu, int selected, const bruce_launcher_theme_t *theme
) {
    int w = display__width();
    int h = display__height();
    if (menu->entry_count == 0) {
        bruce_launcher__draw_centered_text(
            "No entries", (h + BRUCE_LAUNCHER_STATUS_H) / 2, BRUCE_LAUNCHER_FONT_SMALL, theme
        );
        return;
    }

    bool narrow = w < 180;
    int large = narrow ? 52 : 64;
    int small = narrow ? 28 : 36;
    int content_h = h - BRUCE_LAUNCHER_STATUS_H;
    int cy = BRUCE_LAUNCHER_STATUS_H + content_h * 2 / 5;
    int previous = (selected + menu->entry_count - 1) % menu->entry_count;
    int next = (selected + 1) % menu->entry_count;

    const bruce_launcher_entry_t *entries = bruce_launcher__menu_entries(menu);
    if (menu->entry_count > 1) {
        bruce_launcher__draw_entry_icon(&entries[previous], w / 7, cy, small, theme->sec);
        bruce_launcher__draw_entry_icon(&entries[next], w - w / 7, cy, small, theme->sec);
    }
    bruce_launcher__draw_entry_icon(&entries[selected], w / 2, cy, large, theme->pri);

    bruce_launcher__draw_centered_text(
        bruce_launcher__entry_label(&entries[selected]),
        cy + large / 2 + 10,
        BRUCE_LAUNCHER_FONT_MEDIUM,
        theme
    );
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

static void bruce_launcher__draw_sliding_icon(
    const bruce_launcher_menu_t *menu, int from, int relative, int direction, int progress, int center_x,
    int spacing, int cy, int small, int large, const bruce_launcher_theme_t *theme
) {
    int position = relative * BRUCE_LAUNCHER_EASING_SCALE - direction * progress;
    int distance = abs(position);
    if (distance > BRUCE_LAUNCHER_EASING_SCALE) { distance = BRUCE_LAUNCHER_EASING_SCALE; }
    int size = large - (large - small) * distance / BRUCE_LAUNCHER_EASING_SCALE;
    uint16_t color = bruce_launcher__blend_color(theme->pri, theme->sec, distance);
    int x = center_x + position * spacing / BRUCE_LAUNCHER_EASING_SCALE;
    int entry = bruce_launcher__wrap_index(from + relative, menu->entry_count);
    bruce_launcher__draw_entry_icon(&bruce_launcher__menu_entries(menu)[entry], x, cy, size, color);
}

static void bruce_launcher__draw_root_transition(
    const bruce_launcher_menu_t *menu, int from, int direction, int progress,
    const bruce_launcher_theme_t *theme
) {
    int w = display__width();
    int h = display__height();
    bool narrow = w < 180;
    int large = narrow ? 52 : 64;
    int small = narrow ? 28 : 36;
    int cy = BRUCE_LAUNCHER_STATUS_H + (h - BRUCE_LAUNCHER_STATUS_H) * 2 / 5;
    int spacing = w / 2 - w / 7;
    int outer_before = direction > 0 ? -1 : 1;
    int outer_after = direction > 0 ? 2 : -2;
    int destination = direction;

    bruce_launcher__draw_sliding_icon(
        menu, from, outer_before, direction, progress, w / 2, spacing, cy, small, large, theme
    );
    bruce_launcher__draw_sliding_icon(
        menu, from, outer_after, direction, progress, w / 2, spacing, cy, small, large, theme
    );
    if (progress < BRUCE_LAUNCHER_EASING_SCALE / 2) {
        bruce_launcher__draw_sliding_icon(
            menu, from, destination, direction, progress, w / 2, spacing, cy, small, large, theme
        );
        bruce_launcher__draw_sliding_icon(
            menu, from, 0, direction, progress, w / 2, spacing, cy, small, large, theme
        );
    } else {
        bruce_launcher__draw_sliding_icon(
            menu, from, 0, direction, progress, w / 2, spacing, cy, small, large, theme
        );
        bruce_launcher__draw_sliding_icon(
            menu, from, destination, direction, progress, w / 2, spacing, cy, small, large, theme
        );
    }

    int label_entry = progress < BRUCE_LAUNCHER_EASING_SCALE / 2
                          ? from
                          : bruce_launcher__wrap_index(from + direction, menu->entry_count);
    bruce_launcher__draw_centered_text(
        bruce_launcher__entry_label(&bruce_launcher__menu_entries(menu)[label_entry]),
        cy + large / 2 + 10,
        BRUCE_LAUNCHER_FONT_MEDIUM,
        theme
    );
}

static bruce_result_t bruce_launcher__animate_root_menu(
    const bruce_launcher_menu_t *menu, int from, int direction, const bruce_launcher_theme_t *theme,
    uint32_t *icon_revision
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
        bruce_launcher__draw_root_transition(menu, from, direction, progress, theme);
        *icon_revision = bruce_launcher__draw_status_bar(theme);
        frame = display__present();
        if (frame != BRUCE_OK) { return frame; }
        if (linear == BRUCE_LAUNCHER_EASING_SCALE) { return BRUCE_OK; }
    }
}

static size_t bruce_launcher__process_candidates(bruce_process_snapshot_t *processes, size_t capacity) {
    size_t count = 0;
    size_t written = 0;
    bruce_process_id_t self = process__current_id();
    if (process__list(processes, capacity, &count) != BRUCE_OK) { return 0; }
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].id != self && processes[i].presentable &&
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
    size_t page = 0;
    int selected = 0;
    bool redraw = true;
    for (;;) {
        bruce_process_snapshot_t candidates[16];
        size_t total =
            bruce_launcher__process_candidates(candidates, sizeof(candidates) / sizeof(candidates[0]));
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
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) { return 0; }
        if (input_result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) { continue; }
        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B) {
            (void)display__set_tiles(NULL, 0);
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
    char command[BRUCE_LAUNCHER_COMMAND_MAX + 8];
    if (!runtime__gui_requested() || bruce_launcher__command_sets(entry->command, "GUI")) {
        snprintf(command, sizeof(command), "%s", entry->command);
    } else {
        snprintf(command, sizeof(command), "GUI=1 %s", entry->command);
    }
    int result = app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);

    if (result < 0) {
        char message[128];
        snprintf(
            message,
            sizeof(message),
            "Could not start %s: %s",
            bruce_launcher__entry_label(entry),
            app_runner__result_to_string(result)
        );
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Launch failed", message);
    }
    return result;
}

/* -------------------------------------------------------------------------- */
/* GUI menu runner                                                            */
/* -------------------------------------------------------------------------- */

/* The root is a horizontal carousel. Nested menus use Core's choice renderer
 * inside the launcher's status bar and outer border. */
static int bruce_launcher__run_gui_menu(const bruce_launcher_menu_t *menu) {
    bruce_launcher_theme_t theme;
    bruce_launcher__get_theme(&theme);

    if (!menu->is_root) {
        const bruce_launcher_entry_t *entries = bruce_launcher__menu_entries(menu);
        bruce_dialog_choice_t *choices =
            (bruce_dialog_choice_t *)memory__malloc(sizeof(*choices) * (size_t)menu->entry_count);
        if (choices == NULL) return BRUCE_ERR_NO_MEMORY;

        const bruce_dialog_render_params_t render_params = {.window_chrome = true};

        (void)input__flush();
        for (;;) {
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
                memory__free(choices);
                return frame;
            }
            bruce_launcher__draw_main_border(&theme);
            (void)bruce_launcher__draw_status_bar(&theme);
            frame = display__present();
            if (frame != BRUCE_OK) {
                memory__free(choices);
                return frame;
            }

            size_t selected = 0;
            /* Published only across the dialog call: bruce_launcher__window_draw_status()
             * runs on other processes' dialogs too, and must not touch this
             * array once it goes out of scope. */
            s_live_choices.choices = choices;
            s_live_choices.entries = entries;
            s_live_choices.count = menu->entry_count;
            bruce_launcher__refresh_live_choices();
            bruce_result_t result = dialog__choice(
                menu->title, NULL, choices, (size_t)menu->entry_count, &selected, &render_params
            );
            s_live_choices.count = 0;
            if (result == BRUCE_ERR_CANCELLED) break;
            if (result == BRUCE_ERR_NOT_FOREGROUND) {
                (void)runtime__sleep(BRUCE_LAUNCHER_BACKGROUND_WAIT_MS);
                continue;
            }
            if (result != BRUCE_OK) continue;

            const bruce_launcher_entry_t *entry = &entries[selected];
            if (entry->kind == BRUCE_LAUNCHER_ENTRY_BACK) break;
            if (entry->kind == BRUCE_LAUNCHER_ENTRY_SUBMENU) {
                (void)bruce_launcher__run_gui_menu(bruce_launcher__entry_submenu(menu, entry));
            } else {
                (void)bruce_launcher__run_entry(entry);
            }
            (void)input__flush();
        }
        memory__free(choices);
        return 0;
    }

    (void)input__flush();

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
            bruce_launcher__draw_root_menu(menu, selected, &theme);
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
            (void)runtime__sleep(BRUCE_LAUNCHER_BACKGROUND_WAIT_MS);
            continue;
        }
        if (result != BRUCE_OK || ev.action != BRUCE_INPUT_PRESS) { continue; }

        switch (ev.code) {
            case BRUCE_INPUT_CODE_LEFT:
            case BRUCE_INPUT_CODE_UP:
            case BRUCE_INPUT_CODE_PREV:
                if (menu->is_root && menu->entry_count > 0) {
                    int previous = selected;
                    selected = (selected + menu->entry_count - 1) % menu->entry_count;
                    if (menu->entry_count > 1) {
                        result =
                            bruce_launcher__animate_root_menu(menu, previous, -1, &theme, &icon_revision);
                        if (result != BRUCE_OK) { return result; }
                        last_drawn = selected;
                    }
                } else if (selected > 0) {
                    selected--;
                }
                break;
            case BRUCE_INPUT_CODE_RIGHT:
            case BRUCE_INPUT_CODE_DOWN:
            case BRUCE_INPUT_CODE_NEXT:
                if (menu->is_root && menu->entry_count > 0) {
                    int previous = selected;
                    selected = (selected + 1) % menu->entry_count;
                    if (menu->entry_count > 1) {
                        result = bruce_launcher__animate_root_menu(menu, previous, 1, &theme, &icon_revision);
                        if (result != BRUCE_OK) { return result; }
                        last_drawn = selected;
                    }
                } else if (selected + 1 < menu->entry_count) {
                    selected++;
                }
                break;
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
        (bruce_dialog_choice_t *)memory__malloc(sizeof(*choices) * (size_t)menu->capacity);
    if (choices == NULL) { return BRUCE_ERR_NO_MEMORY; }

    for (;;) {
        for (int i = 0; i < menu->entry_count; ++i) {
            choices[i].label = bruce_launcher__entry_label(&entries[i]);
            choices[i].value = choices[i].label;
        }

        size_t choice = 0;
        bruce_result_t choice_result =
            dialog__choice(menu->title, "Select an app", choices, (size_t)menu->entry_count, &choice, NULL);

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
    } else {
        result = bruce_launcher__run_terminal_menu(root);
    }

    bruce_launcher__menu_free(root);
    display__fill_screen(BRUCE_COLOR_BLACK);
    return result;
}
