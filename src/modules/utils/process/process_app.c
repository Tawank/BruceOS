#include "process_app.h"

#include <errno.h> // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"

#define PROCESS_APP__PREVIEW_HEADER_H 25
#define PROCESS_APP__PREVIEW_LABEL_H 12
#define PROCESS_APP__PREVIEW_BORDER 3
#define PROCESS_APP__PREVIEW_LONG_PRESS_MS 500u

static int process_app__parse_id(const char *value, bruce_process_id_t *out_id) {
    if (value == NULL || value[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == BRUCE_PROCESS_ID_INVALID ||
        parsed > UINT32_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_id = (bruce_process_id_t)parsed;
    return BRUCE_OK;
}

static int process_app__switch(const char *target) {
    if (strcmp(target, "next") == 0) return process__switch_next();
    if (strcmp(target, "prev") == 0) return process__switch_previous();

    bruce_process_id_t process_id = BRUCE_PROCESS_ID_INVALID;
    int parsed = process_app__parse_id(target, &process_id);
    return parsed == BRUCE_OK ? process__foreground(process_id) : parsed;
}

static int process_app__resolve_target(const char *target, bruce_process_id_t *out_id) {
    int parsed = process_app__parse_id(target, out_id);
    if (parsed == BRUCE_OK) return BRUCE_OK;
    if (target == NULL || target[0] == '\0' || (target[0] >= '0' && target[0] <= '9')) return parsed;

    /* Snapshots go on the heap, not this process's default-sized stack. */
    bruce_process_snapshot_t *processes = memory__malloc(BRUCE_PROCESS_MAX * sizeof(*processes));
    if (processes == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t count = 0;
    int result = process__list(processes, BRUCE_PROCESS_MAX, &count);
    bruce_process_id_t match = BRUCE_PROCESS_ID_INVALID;
    for (size_t i = 0; result == BRUCE_OK && i < count; ++i) {
        if (strcmp(processes[i].name, target) != 0) continue;
        if (match != BRUCE_PROCESS_ID_INVALID) {
            result = BRUCE_ERR_BUSY;
            break;
        }
        match = processes[i].id;
    }
    memory__free(processes);
    if (result != BRUCE_OK) return result;
    if (match == BRUCE_PROCESS_ID_INVALID) return BRUCE_ERR_NOT_FOUND;
    *out_id = match;
    return BRUCE_OK;
}

static int process_app__kill(const char *target) {
    bruce_process_id_t process_id = BRUCE_PROCESS_ID_INVALID;
    int resolved = process_app__resolve_target(target, &process_id);
    return resolved == BRUCE_OK ? process__kill(process_id) : resolved;
}

static int process_app__signal(const char *signal_name, const char *target) {
    bruce_process_signal_t signal;
    if (strcmp(signal_name, "int") == 0) {
        signal = BRUCE_PROCESS_SIGNAL_INT;
    } else if (strcmp(signal_name, "term") == 0) {
        signal = BRUCE_PROCESS_SIGNAL_TERM;
    } else if (strcmp(signal_name, "kill") == 0) {
        signal = BRUCE_PROCESS_SIGNAL_KILL;
    } else {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_process_id_t process_id = BRUCE_PROCESS_ID_INVALID;
    int resolved = process_app__resolve_target(target, &process_id);
    return resolved == BRUCE_OK ? process__signal(process_id, signal) : resolved;
}

/* -------------------------------------------------------------------------- */
/* preview: live 2x2 tile grid of background GUI apps                         */
/* -------------------------------------------------------------------------- */

/* Collect background GUI processes eligible for a compositor tile. */
static size_t process_app__preview_candidates(bruce_process_snapshot_t *processes, size_t capacity) {
    size_t count = 0;
    size_t written = 0;
    bruce_process_id_t self = process__current_id();
    if (process__list(processes, capacity, &count) != BRUCE_OK) { return 0; }
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].id != self && processes[i].gui_requested &&
            processes[i].state == BRUCE_PROCESS_BACKGROUND) {
            if (written != i) processes[written] = processes[i];
            written++;
        }
    }
    return written;
}

/* Selection frames around each tile; the tile itself sits just inside its frame's border. */
static void process_app__preview_frames(size_t count, bruce_display_rect_t *frames) {
    int w = display__width();
    int h = display__height();
    int top = PROCESS_APP__PREVIEW_HEADER_H + 2;
    int cols = count == 1 || (count == 2 && h > w) ? 1 : 2;
    int rows = (int)((count + (size_t)cols - 1) / (size_t)cols);
    int cell_w = w / cols;
    int cell_h = (h - top) / rows;
    for (size_t i = 0; i < count; ++i) {
        int col = (int)i % cols;
        int row = (int)i / cols;
        int cell_x = col * cell_w;
        int cell_y = top + row * cell_h;
        int right = col == cols - 1 ? w : cell_x + cell_w;
        int bottom = row == rows - 1 ? h : cell_y + cell_h;
        frames[i] = (bruce_display_rect_t){
            .x = cell_x + 1,
            .y = cell_y + PROCESS_APP__PREVIEW_LABEL_H,
            .width = right - cell_x - 2,
            .height = bottom - cell_y - PROCESS_APP__PREVIEW_LABEL_H - 1,
        };
    }
}

static bruce_display_rect_t process_app__preview_tile_rect(bruce_display_rect_t frame) {
    const int border = PROCESS_APP__PREVIEW_BORDER;
    return (bruce_display_rect_t){
        frame.x + border, frame.y + border, frame.width - 2 * border, frame.height - 2 * border
    };
}

static void
process_app__preview_draw_borders(const bruce_display_rect_t *frames, size_t count, int selected) {
    uint16_t pri = config__get_color_primary();
    uint16_t bg = config__get_color_background();
    for (size_t i = 0; i < count; ++i) {
        bruce_display_rect_t f = frames[i];
        uint16_t color = (int)i == selected ? pri : bg;
        for (int b = 0; b < PROCESS_APP__PREVIEW_BORDER; ++b) {
            display__draw_rect(f.x + b, f.y + b, f.width - 2 * b, f.height - 2 * b, color);
        }
    }
}

/* Repaints the whole page and hands out fresh tiles; only needed when the page's apps change. */
static bruce_result_t
process_app__preview_draw_page(const bruce_process_snapshot_t *processes, size_t count, int selected) {
    bruce_result_t result = display__set_tiles(NULL, 0);
    if (result != BRUCE_OK) { return result; }
    result = display__begin_frame();
    if (result != BRUCE_OK) { return result; }

    uint16_t pri = config__get_color_primary();
    uint16_t bg = config__get_color_background();
    display__fill_screen(bg);
    display__set_text_size(1);
    display__set_text_color(pri);
    display__set_text_bg_color(bg);
    display__set_cursor(4, 7);
    display__print("Task Manager  select: open  hold: options  back: exit");
    int header_y = PROCESS_APP__PREVIEW_HEADER_H;
    display__draw_line(0, header_y, display__width() - 1, header_y, pri);
    if (count == 0) {
        display__set_cursor(4, PROCESS_APP__PREVIEW_HEADER_H + 6);
        display__print("No background GUI apps");
        return display__present();
    }

    bruce_display_rect_t frames[BRUCE_DISPLAY_MAX_TILES];
    bruce_display_tile_t tiles[BRUCE_DISPLAY_MAX_TILES];
    process_app__preview_frames(count, frames);
    for (size_t i = 0; i < count; ++i) {
        display__set_cursor(frames[i].x + 3, frames[i].y - PROCESS_APP__PREVIEW_LABEL_H);
        display__print(processes[i].name);
        tiles[i] = (bruce_display_tile_t){processes[i].id, process_app__preview_tile_rect(frames[i])};
    }
    process_app__preview_draw_borders(frames, count, selected);
    result = display__present();
    if (result != BRUCE_OK) { return result; }
    return display__set_tiles(tiles, count);
}

/* Moves the highlight without touching the tiles, so the apps keep their pixels. */
static bruce_result_t process_app__preview_draw_selection(size_t count, int selected) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) { return result; }
    bruce_display_rect_t frames[BRUCE_DISPLAY_MAX_TILES];
    process_app__preview_frames(count, frames);
    process_app__preview_draw_borders(frames, count, selected);
    return display__present();
}

/* Long-press menu for one app; returns true when the user chose to switch to it. */
static bool process_app__preview_menu(const bruce_process_snapshot_t *process) {
    static const bruce_dialog_choice_t choices[] = {
        {.label = "Switch to", .value = "switch"},
        {.label = "Kill", .value = "kill"},
        {.label = "Cancel", .value = "cancel"},
    };
    (void)display__set_tiles(NULL, 0);
    size_t selected = 0;
    if (dialog__choice(process->name, NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected) !=
        BRUCE_OK) {
        return false;
    }
    if (strcmp(choices[selected].value, "kill") == 0) (void)process__kill(process->id);
    return strcmp(choices[selected].value, "switch") == 0;
}

static int process_app__preview_run(bruce_process_snapshot_t *candidates) {
    size_t index = 0;
    bool layout_dirty = true;
    int drawn_selected = -1;
    bruce_process_id_t drawn_ids[BRUCE_DISPLAY_MAX_TILES] = {0};
    size_t drawn_count = 0;

    for (;;) {
        size_t total = process_app__preview_candidates(candidates, BRUCE_PROCESS_MAX);
        if (index >= total) index = total > 0 ? total - 1 : 0;
        size_t start = index - index % BRUCE_DISPLAY_MAX_TILES;
        size_t page_count = total - start;
        if (page_count > BRUCE_DISPLAY_MAX_TILES) page_count = BRUCE_DISPLAY_MAX_TILES;
        int selected = (int)(index - start);

        if (page_count != drawn_count) layout_dirty = true;
        for (size_t i = 0; !layout_dirty && i < page_count; ++i) {
            if (candidates[start + i].id != drawn_ids[i]) layout_dirty = true;
        }

        if (layout_dirty || selected != drawn_selected) {
            bruce_result_t draw =
                layout_dirty ? process_app__preview_draw_page(&candidates[start], page_count, selected)
                             : process_app__preview_draw_selection(page_count, selected);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            if (draw == BRUCE_ERR_NOT_INITIALIZED) {
                printf("process preview: display not available\n");
                return draw;
            }
            if (draw == BRUCE_ERR_NOT_FOREGROUND) { return 0; }
            layout_dirty = false;
            drawn_selected = selected;
            drawn_count = page_count;
            for (size_t i = 0; i < page_count; ++i) { drawn_ids[i] = candidates[start + i].id; }
        }

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, 100);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) { return 0; }
        if (input_result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) { continue; }
        if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_BUTTON_B) return 0;
        if ((event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_PREV ||
             event.code == BRUCE_INPUT_CODE_LEFT) &&
            total > 0) {
            index = (index + total - 1) % total;
        } else if ((event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT ||
                    event.code == BRUCE_INPUT_CODE_RIGHT) &&
                   total > 0) {
            index = (index + 1) % total;
        } else if ((event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A) &&
                   total > 0) {
            bruce_process_snapshot_t target = candidates[index];
            bool switch_to = true;
            if (input__wait_long_press(&event, PROCESS_APP__PREVIEW_LONG_PRESS_MS)) {
                switch_to = process_app__preview_menu(&target);
            }
            bruce_process_snapshot_t snapshot;
            if (switch_to && process__snapshot(target.id, &snapshot) == BRUCE_OK) {
                (void)process__foreground(target.id);
                return 0;
            }
            layout_dirty = true;
        }
    }
}

static int process_app__preview(void) {
    bruce_process_snapshot_t *candidates = memory__malloc(BRUCE_PROCESS_MAX * sizeof(*candidates));
    if (candidates == NULL) return BRUCE_ERR_NO_MEMORY;
    int result = process_app__preview_run(candidates);
    memory__free(candidates);
    return result;
}

int process_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Switch, preview, signal, or force-kill managed processes.");

    ArgParser *switch_command = ap_new_cmd(root, "switch");
    ArgParser *signal_command = ap_new_cmd(root, "signal");
    ArgParser *kill_command = ap_new_cmd(root, "kill");
    ArgParser *preview_command = ap_new_cmd(root, "preview");
    if (switch_command == NULL || signal_command == NULL || kill_command == NULL || preview_command == NULL) {
        ap_free(root);
        return BRUCE_ERR_NO_MEMORY;
    }
    ap_set_helptext(switch_command, "Switch foreground focus: switch <next|prev|id>");
    ap_add_required_arg(switch_command, "target", "next, prev, or a process ID");
    ap_unknown_options_as_args(switch_command);
    ap_set_helptext(signal_command, "Signal a process: signal <int|term|kill> <id|name>");
    ap_add_required_arg(signal_command, "signal", "int, term, or kill");
    ap_add_required_arg(signal_command, "target", "Process ID or exact process name");
    ap_unknown_options_as_args(signal_command);
    ap_set_helptext(kill_command, "Force-kill a process: kill <id|name>");
    ap_add_required_arg(kill_command, "target", "Process ID or exact process name");
    ap_unknown_options_as_args(kill_command);
    ap_set_helptext(preview_command, "Tile background GUI apps in a live preview grid");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION)
            ap_print_help(ap_get_cmd_parser(root) != NULL ? ap_get_cmd_parser(root) : root);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }

    ArgParser *command = ap_get_cmd_parser(root);
    int result;
    if (command == switch_command) {
        result = process_app__switch(ap_get_arg(switch_command, "target"));
    } else if (command == signal_command) {
        result =
            process_app__signal(ap_get_arg(signal_command, "signal"), ap_get_arg(signal_command, "target"));
    } else if (command == kill_command) {
        result = process_app__kill(ap_get_arg(kill_command, "target"));
    } else if (command == preview_command) {
        result = process_app__preview();
    } else {
        ap_print_help(root);
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
