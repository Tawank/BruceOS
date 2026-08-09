#include "process_app.h"

#include <errno.h> // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

#define PROCESS_APP__MAX_PROCESSES 16
#define PROCESS_APP__PREVIEW_HEADER_H 25

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

    bruce_process_snapshot_t processes[PROCESS_APP__MAX_PROCESSES];
    size_t count = 0;
    int result = process__list(processes, PROCESS_APP__MAX_PROCESSES, &count);
    if (result != BRUCE_OK) return result;

    bruce_process_id_t match = BRUCE_PROCESS_ID_INVALID;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(processes[i].name, target) != 0) continue;
        if (match != BRUCE_PROCESS_ID_INVALID) return BRUCE_ERR_BUSY;
        match = processes[i].id;
    }
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

/* Draw the header, per-app labels and selection frames, and compute the tile
 * rectangles for the current page.  The live app content is composited by Core
 * once display__set_tiles() assigns the rectangles. */
static void
process_app__preview_layout(const bruce_process_snapshot_t *processes, size_t count, int selected, bruce_display_tile_t *tiles) {
    uint16_t pri = config__get_theme_primary();
    uint16_t sec = config__get_theme_secondary();
    uint16_t bg = config__get_theme_background();
    int w = display__width();
    int h = display__height();
    int top = PROCESS_APP__PREVIEW_HEADER_H + 2;
    int cols = count == 1 || (count == 2 && h > w) ? 1 : 2;
    int rows = (int)((count + (size_t)cols - 1) / (size_t)cols);

    display__fill_screen(bg);
    display__set_text_size(1);
    display__set_text_color(pri);
    display__set_text_bg_color(bg);
    display__set_cursor(4, 7);
    display__print("Process preview  arrows: move  select: open  back: exit");
    display__draw_line(0, PROCESS_APP__PREVIEW_HEADER_H, w - 1, PROCESS_APP__PREVIEW_HEADER_H, pri);
    if (count == 0) {
        display__set_cursor(4, top + 4);
        display__print("No background GUI apps");
        return;
    }

    int cell_w = w / cols;
    int cell_h = (h - top) / rows;
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
            cell_x + 1, cell_y + 10, right - cell_x - 2, bottom - cell_y - 11,
            (int)i == selected ? pri : sec
        );
    }
}

static bruce_result_t
process_app__preview_draw_page(const bruce_process_snapshot_t *processes, size_t count, int selected) {
    bruce_result_t result = display__set_tiles(NULL, 0);
    if (result != BRUCE_OK) { return result; }
    result = display__begin_frame();
    if (result != BRUCE_OK) { return result; }
    bruce_display_tile_t tiles[BRUCE_DISPLAY_MAX_TILES];
    process_app__preview_layout(processes, count, selected, tiles);
    result = display__present();
    if (result != BRUCE_OK || count == 0) { return result; }
    return display__set_tiles(tiles, count);
}

static int process_app__preview(void) {
    size_t page = 0;
    int selected = 0;
    bool redraw = true;
    bruce_process_id_t drawn_ids[BRUCE_DISPLAY_MAX_TILES] = {0};
    size_t drawn_count = 0;

    for (;;) {
        bruce_process_snapshot_t candidates[PROCESS_APP__MAX_PROCESSES];
        size_t total = process_app__preview_candidates(candidates, PROCESS_APP__MAX_PROCESSES);
        size_t pages = total == 0 ? 1 : (total + BRUCE_DISPLAY_MAX_TILES - 1) / BRUCE_DISPLAY_MAX_TILES;
        if (page >= pages) page = pages - 1;
        size_t start = page * BRUCE_DISPLAY_MAX_TILES;
        size_t page_count = total > start ? total - start : 0;
        if (page_count > BRUCE_DISPLAY_MAX_TILES) page_count = BRUCE_DISPLAY_MAX_TILES;
        if (selected >= (int)page_count) selected = page_count > 0 ? (int)page_count - 1 : 0;

        /* Redraw live as apps enter or leave the background set. */
        if (!redraw && page_count != drawn_count) redraw = true;
        for (size_t i = 0; !redraw && i < page_count; ++i) {
            if (candidates[start + i].id != drawn_ids[i]) redraw = true;
        }

        if (redraw) {
            bruce_result_t draw = process_app__preview_draw_page(&candidates[start], page_count, selected);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            if (draw == BRUCE_ERR_NOT_INITIALIZED) {
                printf("process preview: display not available\n");
                return draw;
            }
            if (draw == BRUCE_ERR_NOT_FOREGROUND) { return 0; }
            redraw = false;
            drawn_count = page_count;
            for (size_t i = 0; i < page_count; ++i) { drawn_ids[i] = candidates[start + i].id; }
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
        } else if (
            (event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT ||
             event.code == BRUCE_INPUT_CODE_RIGHT) &&
            selected + 1 < (int)page_count
        ) {
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
        } else if (
            (event.code == BRUCE_INPUT_CODE_SELECT || event.code == BRUCE_INPUT_CODE_BUTTON_A) &&
            page_count > 0
        ) {
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
        result = process_app__signal(
            ap_get_arg(signal_command, "signal"), ap_get_arg(signal_command, "target")
        );
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
