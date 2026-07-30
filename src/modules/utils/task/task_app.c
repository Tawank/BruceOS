#include "task_app.h"

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
#include "core_sdk/task.h"

#define TASK_APP__MAX_TASKS 16
#define TASK_APP__PREVIEW_HEADER_H 25

static int task_app__parse_id(const char *value, bruce_task_id_t *out_id) {
    if (value == NULL || value[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == BRUCE_TASK_ID_INVALID ||
        parsed > UINT32_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_id = (bruce_task_id_t)parsed;
    return BRUCE_OK;
}

static int task_app__switch(const char *target) {
    if (strcmp(target, "next") == 0) return task__switch_next();
    if (strcmp(target, "prev") == 0) return task__switch_previous();

    bruce_task_id_t task_id = BRUCE_TASK_ID_INVALID;
    int parsed = task_app__parse_id(target, &task_id);
    return parsed == BRUCE_OK ? task__foreground(task_id) : parsed;
}

static int task_app__resolve_kill_target(const char *target, bruce_task_id_t *out_id) {
    int parsed = task_app__parse_id(target, out_id);
    if (parsed == BRUCE_OK) return BRUCE_OK;
    if (target == NULL || target[0] == '\0' || (target[0] >= '0' && target[0] <= '9')) return parsed;

    bruce_task_snapshot_t tasks[TASK_APP__MAX_TASKS];
    size_t count = 0;
    int result = task__list(tasks, TASK_APP__MAX_TASKS, &count);
    if (result != BRUCE_OK) return result;

    bruce_task_id_t match = BRUCE_TASK_ID_INVALID;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(tasks[i].name, target) != 0) continue;
        if (match != BRUCE_TASK_ID_INVALID) return BRUCE_ERR_BUSY;
        match = tasks[i].id;
    }
    if (match == BRUCE_TASK_ID_INVALID) return BRUCE_ERR_NOT_FOUND;
    *out_id = match;
    return BRUCE_OK;
}

static int task_app__kill(const char *target) {
    bruce_task_id_t task_id = BRUCE_TASK_ID_INVALID;
    int resolved = task_app__resolve_kill_target(target, &task_id);
    return resolved == BRUCE_OK ? task__kill(task_id) : resolved;
}

/* -------------------------------------------------------------------------- */
/* preview: live 2x2 tile grid of background GUI apps                         */
/* -------------------------------------------------------------------------- */

/* Collect background GUI tasks eligible for a compositor tile. */
static size_t task_app__preview_candidates(bruce_task_snapshot_t *tasks, size_t capacity) {
    bruce_task_snapshot_t all[TASK_APP__MAX_TASKS];
    size_t count = 0;
    size_t written = 0;
    bruce_task_id_t self = task__current_id();
    if (task__list(all, TASK_APP__MAX_TASKS, &count) != BRUCE_OK) { return 0; }
    for (size_t i = 0; i < count && written < capacity; ++i) {
        if (all[i].id != self && all[i].gui_requested && all[i].state == BRUCE_TASK_BACKGROUND) {
            tasks[written++] = all[i];
        }
    }
    return written;
}

/* Draw the header, per-app labels and selection frames, and compute the tile
 * rectangles for the current page.  The live app content is composited by Core
 * once display__set_tiles() assigns the rectangles. */
static void
task_app__preview_layout(const bruce_task_snapshot_t *tasks, size_t count, int selected, bruce_display_tile_t *tiles) {
    uint16_t pri = config__get_pri_color();
    uint16_t sec = config__get_sec_color();
    uint16_t bg = config__get_bg_color();
    int w = display__width();
    int h = display__height();
    int top = TASK_APP__PREVIEW_HEADER_H + 2;
    int cols = count == 1 || (count == 2 && h > w) ? 1 : 2;
    int rows = (int)((count + (size_t)cols - 1) / (size_t)cols);

    display__fill_screen(bg);
    display__set_text_size(1);
    display__set_text_color(pri);
    display__set_text_bg_color(bg);
    display__set_cursor(4, 7);
    display__print("Task preview  arrows: move  select: open  back: exit");
    display__draw_line(0, TASK_APP__PREVIEW_HEADER_H, w - 1, TASK_APP__PREVIEW_HEADER_H, pri);
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
        tiles[i].task_id = tasks[i].id;
        tiles[i].rect = (bruce_display_rect_t){
            .x = cell_x + 3,
            .y = cell_y + 12,
            .width = right - cell_x - 6,
            .height = bottom - cell_y - 15,
        };
        display__set_cursor(cell_x + 4, cell_y + 2);
        display__print(tasks[i].name);
        display__draw_rect(
            cell_x + 1, cell_y + 10, right - cell_x - 2, bottom - cell_y - 11,
            (int)i == selected ? pri : sec
        );
    }
}

static bruce_result_t
task_app__preview_draw_page(const bruce_task_snapshot_t *tasks, size_t count, int selected) {
    bruce_result_t result = display__set_tiles(NULL, 0);
    if (result != BRUCE_OK) { return result; }
    result = display__begin_frame();
    if (result != BRUCE_OK) { return result; }
    bruce_display_tile_t tiles[BRUCE_DISPLAY_MAX_TILES];
    task_app__preview_layout(tasks, count, selected, tiles);
    result = display__present();
    if (result != BRUCE_OK || count == 0) { return result; }
    return display__set_tiles(tiles, count);
}

static int task_app__preview(bool background) {
    if (!background) {
        bruce_result_t foreground = task__to_foreground();
        if (foreground != BRUCE_OK) { return foreground; }
    }

    size_t page = 0;
    int selected = 0;
    bool redraw = true;
    bruce_task_id_t drawn_ids[BRUCE_DISPLAY_MAX_TILES] = {0};
    size_t drawn_count = 0;

    for (;;) {
        bruce_task_snapshot_t candidates[TASK_APP__MAX_TASKS];
        size_t total = task_app__preview_candidates(candidates, TASK_APP__MAX_TASKS);
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
            bruce_result_t draw = task_app__preview_draw_page(&candidates[start], page_count, selected);
            if (draw == BRUCE_ERR_BUSY) {
                (void)runtime__delay(20);
                continue;
            }
            if (draw == BRUCE_ERR_NOT_INITIALIZED) {
                printf("task preview: display not available\n");
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
        if ((event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_LEFT) && selected > 0) {
            selected--;
            redraw = true;
        } else if (
            (event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_RIGHT) &&
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
            bruce_task_id_t target = candidates[start + (size_t)selected].id;
            bruce_task_snapshot_t snapshot;
            if (task__snapshot(target, &snapshot) == BRUCE_OK) {
                (void)display__set_tiles(NULL, 0);
                (void)task__foreground(target);
                return 0;
            }
            redraw = true;
        }
    }
}

int task_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Switch, preview, or force-kill managed tasks.");

    ArgParser *switch_command = ap_new_cmd(root, "switch");
    ArgParser *kill_command = ap_new_cmd(root, "kill");
    ArgParser *preview_command = ap_new_cmd(root, "preview");
    if (switch_command == NULL || kill_command == NULL || preview_command == NULL) {
        ap_free(root);
        return BRUCE_ERR_NO_MEMORY;
    }
    ap_set_helptext(switch_command, "Switch foreground focus: switch <next|prev|id>");
    ap_add_required_arg(switch_command, "target", "next, prev, or a task ID");
    ap_unknown_options_as_args(switch_command);
    ap_set_helptext(kill_command, "Force-kill a task: kill <id|name>");
    ap_add_required_arg(kill_command, "target", "Task ID or exact task name");
    ap_unknown_options_as_args(kill_command);
    ap_set_helptext(preview_command, "Tile background GUI apps in a live preview grid");
    ap_add_flag(preview_command, "bg");
    ap_set_opt_help(preview_command, "bg", "Do not claim foreground at startup");

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
        result = task_app__switch(ap_get_arg(switch_command, "target"));
    } else if (command == kill_command) {
        result = task_app__kill(ap_get_arg(kill_command, "target"));
    } else if (command == preview_command) {
        result = task_app__preview(app_runner__args_have_background(argc, argv));
    } else {
        ap_print_help(root);
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
