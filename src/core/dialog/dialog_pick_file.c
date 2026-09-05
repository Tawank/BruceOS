#include "dialog_pick_file.h"

#include "dialog_choice.h"
#include "dialog_gui_common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/process/process.h"
#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/display.h"
#include "core_sdk/memory.h"
#include "core_sdk/partition_manager.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"

#define DIALOG__CHAR_W 6
#define DIALOG__CHAR_H 10
#define DIALOG__TEXT_SIZE 1
#define DIALOG__MARGIN 2
/* Vertical breathing room dialog__gui_pick_file() requests (via
 * render_params->list_gap) between its title bar and the first list row -
 * without it, a selected first row's fill paints flush against the title
 * bar above it and the two visually merge into one block. Opt-in per
 * render_params, not a default in dialog__gui_choice(), so plain
 * dialog__choice()/_ex() callers keep their existing flush layout. */
#define DIALOG__LIST_GAP 3

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
        return;
    }
    current_path[1] = '\0';
}

/* Copies `current_path`'s last path component (the directory about to be
 * left via ".."/Back) into `out_name`, so the next loop iteration - once it
 * relists the parent - can re-select that entry instead of defaulting back
 * to the top. Only ever holds the single most recent directory left, not a
 * full history stack, so this re-selection only reaches one level up. */
static void
dialog__pick_file_note_returning_from(const char *current_path, char *out_name, size_t out_name_size) {
    const char *last_slash = strrchr(current_path, '/');
    snprintf(out_name, out_name_size, "%s", last_slash != NULL ? last_slash + 1 : current_path);
}

/* Whole-number binary-prefix size, e.g. 20480 -> "20KiB", 2097152 -> "2MiB".
 * Truncates rather than rounding: plenty precise for a status-bar readout. */
static void dialog__pick_file_format_bytes(uint64_t bytes, char *out, size_t out_size) {
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
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
            if (!storage__internal_mount_point(entries[i].label, mount_point, BRUCE_STORAGE_PATH_MAX))
                continue;
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
 * dialog__pick_file_format_usage() above) into the bottom bar
 * dialog__gui_choice() just filled. Assumes the plain full-screen bordered
 * layout (the only one any current caller uses for the picker) - silently
 * draws nothing usable outside that when the text doesn't fit, rather than
 * fail the dialog. */
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
        dialog__pick_file_note_returning_from(
            ws->current_path, ws->returning_from, sizeof(ws->returning_from)
        );
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
        char (*size_texts)[16] = memory__calloc(count + 1, sizeof(*size_texts));
        char (*icon_texts)[BRUCE_DIALOG_ICON_NAME_MAX] = memory__calloc(count + 1, sizeof(*icon_texts));
        const char **values = memory__malloc((count + 1) * sizeof(const char *));
        if (choices == NULL || size_texts == NULL || icon_texts == NULL || values == NULL) {
            memory__free(entries);
            memory__free(choices);
            memory__free(size_texts);
            memory__free(icon_texts);
            memory__free(values);
            return BRUCE_ERR_NO_MEMORY;
        }

        int choice_count = 0;
        /* Keep an explicit exit row at root too. Besides making the exit
         * discoverable, Filemanager can long-press this row to act on the
         * directory currently being displayed. */
        values[choice_count] = "..";
        choices[choice_count].label = strcmp(ws->current_path, "/") == 0 ? "[..] (exit)" : "[..]";
        choices[choice_count].value = "..";
        choices[choice_count].icon_name = "folder-open";
        choice_count++;
        for (size_t i = 0; i < count && (size_t)choice_count < count + 1; ++i) {
            if (entries[i].type == BRUCE_STORAGE_ENTRY_FILE &&
                !dialog__matches_extension_filter(entries[i].name, extension_filter)) {
                continue;
            }
            values[choice_count] = entries[i].name;
            choices[choice_count].label = entries[i].name;
            choices[choice_count].value = entries[i].name;
            bool has_icon_override = false;
            if (effective_params->icon_for_path != NULL) {
                /* Sized for the true worst case (current_path up to
                 * BRUCE_STORAGE_PATH_MAX-1 chars, "/", entries[i].name up to
                 * BRUCE_STORAGE_NAME_MAX-1 chars, NUL) rather than
                 * BRUCE_STORAGE_PATH_MAX itself: unlike every other path this
                 * function builds, current_path here isn't already known to
                 * leave room for one more component, so a same-size buffer
                 * risks a genuine (not just GCC-format-truncation-checker-
                 * blind) truncation deep enough in the tree. Truncation would
                 * still be harmless -- entry_path is only used for an exact-
                 * match icon_for_path() lookup below -- but sizing it to
                 * always fit is simpler than reasoning about that. */
                char entry_path[BRUCE_STORAGE_PATH_MAX + BRUCE_STORAGE_NAME_MAX];
                if (strcmp(ws->current_path, "/") == 0) {
                    snprintf(entry_path, sizeof(entry_path), "/%s", entries[i].name);
                } else {
                    snprintf(entry_path, sizeof(entry_path), "%s/%s", ws->current_path, entries[i].name);
                }
                has_icon_override = effective_params->icon_for_path(
                    entry_path, entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY, icon_texts[choice_count],
                    sizeof(icon_texts[choice_count]), effective_params->icon_for_path_context
                );
            }
            choices[choice_count].icon_name =
                has_icon_override ? icon_texts[choice_count]
                : entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY ? "folder"
                                                                    : app_runner__icon_for_path(entries[i].name);
            if (entries[i].type == BRUCE_STORAGE_ENTRY_FILE) {
                dialog__pick_file_format_bytes(
                    entries[i].size, size_texts[choice_count], sizeof(size_texts[choice_count])
                );
                choices[choice_count].right_text = size_texts[choice_count];
            }
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
            ws->bar_title, NULL, choices, (size_t)choice_count, &out_selected, effective_params, NULL
        );
        effective_params->skip_initial_flush = false; /* one-shot: only meant for the call just made */

        const char *picked = values[out_selected];
        /* Remembered so a spurious cancel from briefly losing foreground
         * (dialog__pick_file_resume_after_handoff() below) can redraw this
         * same listing with the same row still selected instead of
         * resetting to the top - overwritten below by the more specific
         * up-navigation/descend cases when those actually apply. */
        snprintf(ws->returning_from, sizeof(ws->returning_from), "%s", picked);
        memory__free(values);
        memory__free(size_texts);
        memory__free(icon_texts);
        memory__free(choices);

        if (choice_result != BRUCE_OK) {
            memory__free(entries);
            /* Foreground was lost (e.g. alt-tab, the system menu, ...) and
             * has now returned: redraw the same directory instead of
             * unwinding the picker, and skip the next call's usual flush so
             * a follow-up key already queued (e.g. the system menu's
             * injected Back press for its "Esc" button) survives into that
             * redraw instead of being silently discarded. */
            if (dialog__pick_file_resume_after_handoff()) {
                effective_params->skip_initial_flush = true;
                continue;
            }
            /* Genuine Back/Esc: step up a directory rather than exiting the
             * picker outright, unless already at the root. */
            if (strcmp(ws->current_path, "/") != 0) {
                dialog__pick_file_note_returning_from(
                    ws->current_path, ws->returning_from, sizeof(ws->returning_from)
                );
                dialog__pick_file_go_up(ws->current_path);
                continue;
            }
            return BRUCE_ERR_CANCELLED;
        }

        if (strcmp(picked, "..") == 0) {
            if (effective_params->long_press_enabled && effective_params->out_long_press != NULL &&
                *effective_params->out_long_press) {
                if (effective_params->out_parent_entry != NULL) {
                    *effective_params->out_parent_entry = true;
                }
                snprintf(out_path, out_path_size, "%s", ws->current_path);
                memory__free(entries);
                return BRUCE_OK;
            }
            if (strcmp(ws->current_path, "/") == 0) {
                memory__free(entries);
                return BRUCE_ERR_CANCELLED;
            }
            dialog__pick_file_note_returning_from(
                ws->current_path, ws->returning_from, sizeof(ws->returning_from)
            );
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

        /* A long press on a directory row returns it, the same way a plain
         * press on a file does, instead of descending into it - see
         * dialog__pick_file_ex()'s doc comment. */
        if (effective_params->long_press_enabled && effective_params->out_long_press != NULL &&
            *effective_params->out_long_press) {
            snprintf(out_path, out_path_size, "%s", ws->next_path);
            memory__free(entries);
            return BRUCE_OK;
        }

        snprintf(ws->current_path, sizeof(ws->current_path), "%s", ws->next_path);
        ws->returning_from[0] = '\0';
        memory__free(entries);
    }
}

bruce_result_t dialog__gui_pick_file(
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

    /* Also own out_long_press: dialog__gui_pick_file_run() needs to read it
     * after every dialog__gui_choice() call to decide whether a long press
     * on a directory should stop the picker there instead of descending, so
     * it must always get a non-NULL slot to write into even when the
     * original caller passed NULL (wanting long_press_enabled's folder
     * behavior without caring which press kind produced the result). The
     * caller's own slot, if any, is filled in from it below. */
    bool *caller_out_long_press = effective_params.out_long_press;
    bool *caller_out_parent_entry = effective_params.out_parent_entry;
    bool picker_long_press = false;
    bool picker_parent_entry = false;
    if (effective_params.long_press_enabled) {
        effective_params.out_long_press = &picker_long_press;
        effective_params.out_parent_entry = &picker_parent_entry;
    }

    dialog__pick_file_workspace_t *ws = memory__malloc(sizeof(*ws));
    if (ws == NULL) { return BRUCE_ERR_NO_MEMORY; }
    bruce_result_t result = dialog__gui_pick_file_run(
        ws, initial_path, extension_filter, out_path, out_path_size, title, &effective_params
    );
    memory__free(ws);
    if (caller_out_long_press != NULL) { *caller_out_long_press = picker_long_press; }
    if (caller_out_parent_entry != NULL) { *caller_out_parent_entry = picker_parent_entry; }
    return result;
}
