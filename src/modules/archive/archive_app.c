#include "archive_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/archive.h"
#include "core_sdk/dialog.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"

/*
 * GUI (and, automatically, terminal - see core_sdk/dialog.h's module doc
 * comment: rendering is chosen from this process's own launch context, not
 * decided here) browser for a ".zip"/".tar.gz"/".tgz" archive, in the same
 * "look of the filemanager" as filemanager_app.c: full-bleed
 * dialog__choice_ex() listings styled with dialog__default_render_params(),
 * a synthetic "[..]" row to step back out, and long-press for a directory's
 * own action menu instead of descending into it - filemanager_app.c can't
 * be reused directly for this (dialog__pick_file_ex() is storage__list()-
 * backed only), so this rebuilds the same shape one level down, over an
 * archive's own (necessarily flat - see core_sdk/archive.h) entry listing
 * instead.
 *
 * This is what filetype's "program": "archive" opens a ".zip"/".tar.gz"/
 * ".tgz" file with (see embedded_resources/json/extensions.json and
 * filemanager_app.c's "Open" action) - so stepping "up" past the archive's
 * own root hands control right back to whatever launched this (typically
 * filemanager, back to its own view of the real filesystem), the same way
 * filemanager's own picker exits at "/". A file's action menu (Open/View/
 * Extract) mirrors filemanager's, minus the entries that don't make sense
 * for a read-only archive view (Edit/Copy/Rename/Delete); a directory's
 * long-press menu (Extract/Info) mirrors filemanager's folder_actions the
 * same way.
 */

#define ARCHIVE_APP_PREVIEW_MAX 4096
#define ARCHIVE_APP_PREVIEW_SLACK 32

/* An archive-relative path (bruce_archive_entry_t.name, or a prefix of one
 * built by joining path components back together) is always shorter than
 * the full entry name it came from, so BRUCE_ARCHIVE_ENTRY_NAME_MAX is a
 * safe bound for one of these too. */
#define ARCHIVE_APP_PATH_MAX BRUCE_ARCHIVE_ENTRY_NAME_MAX

/* ------------------------------------------------------------------------ */
/* Loading the archive's full (flat) entry list                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    bruce_archive_entry_t *entries;
    size_t count;
    size_t capacity;
    bruce_result_t error;
} archive_app__entries_t;

/* archive__zip_list()/archive__tar_gz_list() both treat a `false` return
 * here as "caller asked to stop early - not a failure" (see
 * core_sdk/archive.h), so an allocation failure below is recorded in
 * `entries->error` instead of trusting either list function's own return
 * value to carry it back out. */
static bool archive_app__collect_entry(void *context, const bruce_archive_entry_t *entry) {
    archive_app__entries_t *entries = (archive_app__entries_t *)context;
    if (entries->count == entries->capacity) {
        size_t new_capacity = entries->capacity == 0 ? 32 : entries->capacity * 2;
        bruce_archive_entry_t *grown = memory__realloc(entries->entries, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            entries->error = BRUCE_ERR_NO_MEMORY;
            return false;
        }
        entries->entries = grown;
        entries->capacity = new_capacity;
    }
    entries->entries[entries->count++] = *entry;
    return true;
}

static bruce_result_t archive_app__load_entries(const char *archive_path, bool zip, archive_app__entries_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->capacity = 0;
    out->error = BRUCE_OK;

    bruce_result_t result = zip ? archive__zip_list(archive_path, archive_app__collect_entry, out)
                                 : archive__tar_gz_list(archive_path, archive_app__collect_entry, out);
    if (result == BRUCE_OK && out->error != BRUCE_OK) result = out->error;
    if (result != BRUCE_OK) {
        memory__free(out->entries);
        out->entries = NULL;
        out->count = 0;
        out->capacity = 0;
    }
    return result;
}

typedef struct {
    const char *archive_path;
    bool zip;
    archive_app__entries_t *out;
    bruce_result_t result;
} archive_app__load_poll_t;

/* archive__zip_list()/archive__tar_gz_list() are one opaque blocking call
 * with no natural pause point to resume from later (see core_sdk/archive.h -
 * a ".tar.gz" even decompresses to a scratch file first), unlike
 * wifi__scan_poll()/bluetooth__scan_poll(). So this poll callback just does
 * the whole load on its one tick; what's actually on screen the whole time
 * is the dialog__message_show() the caller already drew before starting
 * this, unchanged from before - this only swaps the "block right after
 * showing it" mechanism for the same dialog__choice_poll_launcher() shape
 * wifi/bluetooth scanning uses. */
static bruce_result_t archive_app__poll_load(void *context, bool *out_complete) {
    archive_app__load_poll_t *load = context;
    load->result = archive_app__load_entries(load->archive_path, load->zip, load->out);
    *out_complete = true;
    return BRUCE_OK;
}

/* ------------------------------------------------------------------------ */
/* Deriving one directory level's children from the flat entry list         */
/* ------------------------------------------------------------------------ */

typedef struct {
    char name[BRUCE_ARCHIVE_ENTRY_NAME_MAX]; /* This child's own name - no slashes, no `prefix`. */
    bool is_directory;
    size_t size; /* Meaningful only when !is_directory. */
} archive_app__child_t;

/* Builds the list of immediate children of `prefix` ("" for the archive
 * root, otherwise ending in '/') by scanning every entry once. A directory
 * that has no explicit entry of its own (some zip writers only ever store
 * file entries, leaving intermediate directories implicit in their paths)
 * is still inferred and listed exactly once - matching what
 * archive__zip_create()/archive__tar_gz_create() always do write, so an
 * archive built by this codebase and one built elsewhere browse the same
 * way. */
static bruce_result_t archive_app__list_children(
    const bruce_archive_entry_t *entries, size_t entry_count, const char *prefix,
    archive_app__child_t **out_children, size_t *out_count
) {
    size_t prefix_len = strlen(prefix);
    archive_app__child_t *children = NULL;
    size_t count = 0;
    size_t capacity = 0;
    bruce_result_t result = BRUCE_OK;

    for (size_t i = 0; i < entry_count && result == BRUCE_OK; ++i) {
        const char *name = entries[i].name;
        if (strncmp(name, prefix, prefix_len) != 0) continue;
        const char *remainder = name + prefix_len;
        if (remainder[0] == '\0') continue; /* `prefix` naming itself, not a child of it */

        const char *slash = strchr(remainder, '/');
        bool child_is_directory = slash != NULL;
        size_t child_name_len = child_is_directory ? (size_t)(slash - remainder) : strlen(remainder);
        if (child_name_len == 0 || child_name_len >= sizeof(children[0].name)) continue; /* defensive */

        bool duplicate = false;
        for (size_t j = 0; j < count && !duplicate; ++j) {
            duplicate = children[j].is_directory && child_is_directory &&
                        strncmp(children[j].name, remainder, child_name_len) == 0 &&
                        children[j].name[child_name_len] == '\0';
        }
        if (duplicate) continue;

        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
            archive_app__child_t *grown = memory__realloc(children, new_capacity * sizeof(*grown));
            if (grown == NULL) {
                result = BRUCE_ERR_NO_MEMORY;
                break;
            }
            children = grown;
            capacity = new_capacity;
        }
        snprintf(children[count].name, sizeof(children[count].name), "%.*s", (int)child_name_len, remainder);
        children[count].is_directory = child_is_directory;
        children[count].size = child_is_directory ? 0 : entries[i].size;
        count++;
    }

    if (result != BRUCE_OK) {
        memory__free(children);
        return result;
    }
    *out_children = children;
    *out_count = count;
    return BRUCE_OK;
}

static int archive_app__compare_children(const void *a, const void *b) {
    const archive_app__child_t *child_a = (const archive_app__child_t *)a;
    const archive_app__child_t *child_b = (const archive_app__child_t *)b;
    if (child_a->is_directory != child_b->is_directory) return child_a->is_directory ? -1 : 1;
    return strcasecmp(child_a->name, child_b->name);
}

/* Pops the last path component off a trailing-'/'-terminated `prefix`
 * ("docs/notes/" -> "docs/", "docs/" -> ""). Only ever called when `prefix`
 * isn't already "" (the caller exits the browser instead of going up
 * further at the root - see archive_app_main()). */
static void archive_app__go_up(char *prefix) {
    size_t len = strlen(prefix);
    if (len > 0 && prefix[len - 1] == '/') len--;
    while (len > 0 && prefix[len - 1] != '/') len--;
    prefix[len] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Small shared helpers                                                     */
/* ------------------------------------------------------------------------ */

static bool archive_app__is_zip(const char *path) {
    size_t length = strlen(path);
    return length >= 4 && strcasecmp(path + length - 4, ".zip") == 0;
}

static const char *archive_app__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* `dest_dir` default for extracting: the archive's own containing
 * directory - same "extract here" convention bnu__archive_dirname()
 * (bnu_archive_app.c) uses for the whole-archive case. */
static void archive_app__parent_dir(const char *path, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == NULL || slash == out) snprintf(out, out_size, "/");
    else *slash = '\0';
}

static void archive_app__format_bytes(size_t bytes, char *out, size_t out_size) {
    static const char *const units[] = {"B", "KB", "MB", "GB"};
    size_t unit = 0;
    size_t divisor = 1;
    while (unit + 1 < sizeof(units) / sizeof(units[0]) && bytes >= divisor * 1024) {
        divisor *= 1024;
        unit++;
    }
    snprintf(out, out_size, "%zu%s", bytes / divisor, units[unit]);
}

static void archive_app__show_error(const char *action, bruce_result_t result) {
    char message[96];
    snprintf(message, sizeof(message), "%s failed: %s", action, result__to_string(result));
    (void)dialog__message(BRUCE_DIALOG_ERROR, "Archive", message);
}

/* A dialog__choice_ex()/dialog__text_input() call losing (and this process
 * later regaining) foreground - e.g. the launcher's own switcher, or a
 * child process this app spawned via "Open" taking it - surfaces as
 * BRUCE_ERR_CANCELLED same as a genuine Back/Esc; this tells them apart the
 * same way filemanager_app.c's identical helper does, so a real handoff
 * redraws instead of being treated as the user backing out. */
static bool archive_app__resume_after_handoff(void) {
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

/* ------------------------------------------------------------------------ */
/* "View": read one entry into memory and show it, nothing written to disk  */
/* ------------------------------------------------------------------------ */

/* Same scroll/resize/close loop as filemanager_app.c's filemanager__view_file()
 * tail (once its viewer already exists) - kept as its own copy since
 * filemanager_app.c doesn't export it, not because the behavior should ever
 * drift between the two. */
static bruce_result_t archive_app__run_viewer(bruce_viewer_id_t viewer) {
    (void)input__flush();
    int text_size = 1;
    for (;;) {
        bruce_input_event_t event;
        bruce_result_t result = input__read(&event, 100);
        if (result == BRUCE_ERR_NOT_FOREGROUND && archive_app__resume_after_handoff()) continue;
        if (result == BRUCE_ERR_NOT_FOREGROUND) break;
        if (result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) continue;

        if (event.type == BRUCE_INPUT_KEY && event.code == '-' && text_size > 1) {
            (void)dialog__viewer_set_text_size(viewer, --text_size);
        } else if (event.type == BRUCE_INPUT_KEY && event.code == '=' && text_size < 8) {
            (void)dialog__viewer_set_text_size(viewer, ++text_size);
        } else if (event.code == BRUCE_INPUT_CODE_UP || event.code == BRUCE_INPUT_CODE_PREV) {
            (void)dialog__viewer_scroll(viewer, -1);
        } else if (event.code == BRUCE_INPUT_CODE_DOWN || event.code == BRUCE_INPUT_CODE_NEXT) {
            (void)dialog__viewer_scroll(viewer, 1);
        } else if (event.code == BRUCE_INPUT_CODE_LEFT) {
            (void)dialog__viewer_scroll(viewer, -5);
        } else if (event.code == BRUCE_INPUT_CODE_RIGHT) {
            (void)dialog__viewer_scroll(viewer, 5);
        } else if (event.code == BRUCE_INPUT_CODE_BACK || event.code == BRUCE_INPUT_CODE_SELECT) {
            break;
        }
    }
    return dialog__viewer_close(viewer);
}

static bruce_result_t
archive_app__view_entry(const char *archive_path, bool zip, const char *entry_name, const char *label) {
    size_t buffer_size = ARCHIVE_APP_PREVIEW_MAX + ARCHIVE_APP_PREVIEW_SLACK;
    char *text = memory__malloc(buffer_size);
    if (text == NULL) return BRUCE_ERR_NO_MEMORY;

    size_t full_size = 0;
    bruce_result_t result = zip
        ? archive__zip_read_entry(archive_path, entry_name, text, ARCHIVE_APP_PREVIEW_MAX + 1, &full_size)
        : archive__tar_gz_read_entry(archive_path, entry_name, text, ARCHIVE_APP_PREVIEW_MAX + 1, &full_size);
    if (result != BRUCE_OK) {
        memory__free(text);
        return result;
    }

    size_t shown = strlen(text);
    for (size_t i = 0; i < shown; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') text[i] = '.';
    }
    if (full_size > shown) snprintf(text + shown, buffer_size - shown, "\n[preview truncated]");

    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    result = dialog__create_text_viewer(label, text, &viewer);
    memory__free(text);
    if (result != BRUCE_OK) return result;
    return archive_app__run_viewer(viewer);
}

/* ------------------------------------------------------------------------ */
/* "Extract": a single entry (file, or a directory's whole subtree)         */
/* ------------------------------------------------------------------------ */

static bruce_result_t
archive_app__extract_entry(const char *archive_path, bool zip, const char *entry_name, const char *label) {
    char dest_default[BRUCE_STORAGE_PATH_MAX];
    archive_app__parent_dir(archive_path, dest_default, sizeof(dest_default));

    char prompt[ARCHIVE_APP_PATH_MAX + 24];
    snprintf(prompt, sizeof(prompt), "Extract \"%s\" to", label);

    char dest[BRUCE_STORAGE_PATH_MAX];
    bruce_result_t result = dialog__text_input("Extract", prompt, dest_default, false, dest, sizeof(dest));
    if (result != BRUCE_OK) return result; /* cancelled */
    if (dest[0] != '/') return BRUCE_ERR_INVALID_PATH;

    result = storage__mkdir(dest);
    if (result != BRUCE_OK) return result;

    result = zip ? archive__zip_extract_entry(archive_path, entry_name, dest)
                 : archive__tar_gz_extract_entry(archive_path, entry_name, dest);
    if (result == BRUCE_OK) (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Archive", "Extracted");
    return result;
}

/* ------------------------------------------------------------------------ */
/* "Open": extract one file to a scratch location and hand it to its app    */
/* ------------------------------------------------------------------------ */

/* Removes `extracted_path` (a file) and then walks back up through its
 * now-empty parent directories, stopping at the first storage__remove()
 * that fails (a directory that wasn't actually empty - shouldn't happen,
 * since this scratch tree only ever holds the one file just removed and
 * whatever parent directories archive__*_extract_entry() created for it) or
 * once `scratch_root` itself has been removed. Best-effort: like any other
 * temp-file cleanup, a leftover empty directory here is harmless, just
 * untidy. */
static void archive_app__cleanup_scratch(const char *scratch_root, const char *extracted_path) {
    char path[BRUCE_STORAGE_PATH_MAX];
    snprintf(path, sizeof(path), "%s", extracted_path);
    (void)storage__remove(path);
    size_t scratch_root_len = strlen(scratch_root);
    for (;;) {
        char *slash = strrchr(path, '/');
        if (slash == NULL || slash == path || strlen(path) < scratch_root_len) break;
        *slash = '\0';
        bool at_root = strcmp(path, scratch_root) == 0;
        if (storage__remove(path) != BRUCE_OK || at_root) break;
    }
}

static bruce_result_t
archive_app__open_entry(const char *archive_path, bool zip, const char *entry_name, bool gui) {
    char scratch_root[BRUCE_STORAGE_PATH_MAX];
    int written = snprintf(scratch_root, sizeof(scratch_root), "%s.tmp", archive_path);
    if (written < 0 || (size_t)written >= sizeof(scratch_root)) return BRUCE_ERR_RESOURCE_LIMIT;

    bruce_result_t result = storage__mkdir(scratch_root);
    if (result != BRUCE_OK) return result;

    result = zip ? archive__zip_extract_entry(archive_path, entry_name, scratch_root)
                 : archive__tar_gz_extract_entry(archive_path, entry_name, scratch_root);
    if (result != BRUCE_OK) {
        (void)storage__remove(scratch_root);
        return result;
    }

    char extracted_path[BRUCE_STORAGE_PATH_MAX];
    written = snprintf(extracted_path, sizeof(extracted_path), "%s/%s", scratch_root, entry_name);
    if (written < 0 || (size_t)written >= sizeof(extracted_path)) {
        (void)storage__remove(scratch_root);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    const bruce_environment_variable_t gui_env[] = {
        {.name = "GUI", .value = "1"}
    };
    int process = app_runner__run_path_with_environment(
        extracted_path, NULL, BRUCE_LAUNCH_FOREGROUND, gui ? gui_env : NULL, gui ? 1u : 0u
    );
    result = process > 0 ? process__wait((bruce_process_id_t)process, UINT32_MAX) : (bruce_result_t)process;

    archive_app__cleanup_scratch(scratch_root, extracted_path);
    return result;
}

/* ------------------------------------------------------------------------ */
/* Per-item action menus                                                    */
/* ------------------------------------------------------------------------ */

static bruce_result_t archive_app__show_folder_info(
    const archive_app__entries_t *entries, const char *full_path, const char *label
) {
    size_t full_path_len = strlen(full_path);
    size_t item_count = 0;
    size_t total_size = 0;
    for (size_t i = 0; i < entries->count; ++i) {
        const char *name = entries->entries[i].name;
        if (strncmp(name, full_path, full_path_len) != 0 || name[full_path_len] != '/') continue;
        if (entries->entries[i].is_directory) continue;
        item_count++;
        total_size += entries->entries[i].size;
    }

    char size_text[16];
    archive_app__format_bytes(total_size, size_text, sizeof(size_text));
    char message[ARCHIVE_APP_PATH_MAX + 48];
    snprintf(
        message, sizeof(message), "%s\n%zu item%s, %s", label, item_count, item_count == 1 ? "" : "s", size_text
    );
    return dialog__message(BRUCE_DIALOG_INFO, "Folder info", message);
}

static bruce_result_t archive_app__folder_menu(
    const archive_app__entries_t *entries, const char *archive_path, bool zip, const char *full_path,
    const char *label
) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Extract", .value = "extract"},
        {.label = "Info",    .value = "info"   },
        {.label = "Back",    .value = "back"   },
    };
    size_t selected = 0;
    bruce_result_t result;
    do {
        result = dialog__choice(label, NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected);
    } while (result == BRUCE_ERR_CANCELLED && archive_app__resume_after_handoff());
    if (result != BRUCE_OK) return result;

    const char *action = choices[selected].value;
    if (strcmp(action, "extract") == 0) return archive_app__extract_entry(archive_path, zip, full_path, label);
    if (strcmp(action, "info") == 0) return archive_app__show_folder_info(entries, full_path, label);
    return BRUCE_OK; /* back */
}

static bruce_result_t
archive_app__file_menu(const char *archive_path, bool zip, bool gui, const char *full_path, const char *label) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Open",    .value = "open"   },
        {.label = "View",    .value = "view"   },
        {.label = "Extract", .value = "extract"},
        {.label = "Back",    .value = "back"   },
    };
    size_t selected = 0;
    bruce_result_t result;
    do {
        result = dialog__choice(label, NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected);
    } while (result == BRUCE_ERR_CANCELLED && archive_app__resume_after_handoff());
    if (result != BRUCE_OK) return result;

    const char *action = choices[selected].value;
    if (strcmp(action, "open") == 0) return archive_app__open_entry(archive_path, zip, full_path, gui);
    if (strcmp(action, "view") == 0) return archive_app__view_entry(archive_path, zip, full_path, label);
    if (strcmp(action, "extract") == 0) return archive_app__extract_entry(archive_path, zip, full_path, label);
    return BRUCE_OK; /* back */
}

/* ------------------------------------------------------------------------ */
/* Main browsing loop                                                       */
/* ------------------------------------------------------------------------ */

int archive_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Browse a .zip or .tar.gz/.tgz archive's contents.");
    ap_add_required_arg(parser, "archive", "Archive file to browse");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    char archive_path[BRUCE_STORAGE_PATH_MAX];
    int path_length = snprintf(archive_path, sizeof(archive_path), "%s", ap_get_arg(parser, "archive"));
    ap_free(parser);
    if (path_length < 0 || (size_t)path_length >= sizeof(archive_path)) return BRUCE_ERR_INVALID_ARGUMENT;

    bool zip = archive_app__is_zip(archive_path);
    bool gui = runtime__gui_requested();
    const char *basename = archive_app__basename(archive_path);

    archive_app__entries_t entries;
    bruce_result_t result;
    if (gui) {
        char message[BRUCE_STORAGE_PATH_MAX + 16];
        snprintf(message, sizeof(message), "Loading...\n%s", basename);
        (void)dialog__message_show(BRUCE_DIALOG_INFO, "Archive", message);

        archive_app__load_poll_t load = {.archive_path = archive_path, .zip = zip, .out = &entries, .result = BRUCE_OK};
        const bruce_dialog_choice_t choices[] = {
            {.label = "Back", .value = "back"},
        };
        size_t selected = 0;
        bool complete = false;
        result = dialog__choice_poll_launcher(
            "Archive", NULL, choices, sizeof(choices) / sizeof(choices[0]), 1000, archive_app__poll_load, &load,
            NULL, &selected, &complete
        );
        if (result == BRUCE_OK) result = load.result;
    } else {
        result = archive_app__load_entries(archive_path, zip, &entries);
    }
    if (result != BRUCE_OK) {
        archive_app__show_error("Open archive", result);
        return result;
    }

    char prefix[ARCHIVE_APP_PATH_MAX] = "";
    for (;;) {
        archive_app__child_t *children = NULL;
        size_t child_count = 0;
        result = archive_app__list_children(entries.entries, entries.count, prefix, &children, &child_count);
        if (result != BRUCE_OK) {
            archive_app__show_error("List", result);
            break;
        }
        qsort(children, child_count, sizeof(*children), archive_app__compare_children);

        bool at_root = prefix[0] == '\0';
        size_t row_count = child_count + 1; /* + the synthetic "[..]" row */
        bruce_dialog_choice_t *choices = memory__calloc(row_count, sizeof(*choices));
        char (*right_texts)[16] = memory__calloc(row_count, sizeof(*right_texts));
        if (choices == NULL || right_texts == NULL) {
            memory__free(children);
            memory__free(choices);
            memory__free(right_texts);
            archive_app__show_error("List", BRUCE_ERR_NO_MEMORY);
            break;
        }

        choices[0].label = at_root ? "[..] (exit)" : "[..]";
        choices[0].value = "..";
        choices[0].icon_name = "folder-open";
        for (size_t i = 0; i < child_count; ++i) {
            choices[i + 1].label = children[i].name;
            choices[i + 1].value = children[i].name;
            choices[i + 1].icon_name =
                children[i].is_directory ? "folder" : app_runner__icon_for_path(children[i].name);
            if (!children[i].is_directory) {
                archive_app__format_bytes(children[i].size, right_texts[i + 1], sizeof(right_texts[i + 1]));
                choices[i + 1].right_text = right_texts[i + 1];
            }
        }

        char bar_title[BRUCE_STORAGE_PATH_MAX + ARCHIVE_APP_PATH_MAX + 4];
        snprintf(bar_title, sizeof(bar_title), "%s - /%s", basename, prefix);

        bruce_dialog_render_params_t render_params = dialog__default_render_params(2);
        render_params.long_press_enabled = true;
        bool long_press = false;
        render_params.out_long_press = &long_press;

        size_t selected = 0;
        result = dialog__choice_ex(bar_title, NULL, choices, row_count, &selected, &render_params);
        memory__free(choices);
        memory__free(right_texts);

        if (result == BRUCE_ERR_CANCELLED && archive_app__resume_after_handoff()) {
            memory__free(children);
            (void)input__flush();
            continue;
        }
        /* A genuine Back/Esc is treated exactly like picking "[..]" below. */
        bool went_back = result == BRUCE_ERR_CANCELLED;
        if (result != BRUCE_OK && !went_back) {
            memory__free(children);
            archive_app__show_error("Browse", result);
            break;
        }

        if (went_back || selected == 0) {
            memory__free(children);
            if (at_root) break; /* hands control back to whatever launched this (filemanager, shell, ...) */
            archive_app__go_up(prefix);
            (void)input__flush();
            continue;
        }

        archive_app__child_t picked = children[selected - 1];
        memory__free(children);

        /* Sized (and precision-capped below) at double ARCHIVE_APP_PATH_MAX
         * rather than just ARCHIVE_APP_PATH_MAX: `prefix` and `picked.name`
         * are each already bounded well under that by construction (both
         * ultimately slices of one archive__*_list() entry name, always
         * < BRUCE_ARCHIVE_ENTRY_NAME_MAX), but GCC can only see their
         * declared array sizes, not that invariant, so it assumes the
         * pessimistic 99+99 case - giving full_path headroom for that
         * (instead of just asserting the invariant via a fixed literal
         * precision) is what -Wformat-truncation needs to see this can
         * never actually truncate. */
        char full_path[ARCHIVE_APP_PATH_MAX * 2];
        snprintf(full_path, sizeof(full_path), "%.99s%.99s", prefix, picked.name);

        if (picked.is_directory && !long_press) {
            /* Guards against a full_path that's somehow grown past what a
             * single archive entry's name could ever be (should be
             * unreachable given the invariant above) - silently refusing to
             * descend is safer than the alternative of continuing anyway
             * with a `prefix` this ".98s" precision may have quietly
             * truncated, which the entry-matching below has no way to
             * detect after the fact. */
            if (strlen(full_path) + 2 <= sizeof(prefix)) snprintf(prefix, sizeof(prefix), "%.98s/", full_path);
            (void)input__flush();
            continue;
        }

        (void)input__flush();
        result = picked.is_directory
                     ? archive_app__folder_menu(&entries, archive_path, zip, full_path, picked.name)
                     : archive_app__file_menu(archive_path, zip, gui, full_path, picked.name);
        if (result != BRUCE_OK && result != BRUCE_ERR_CANCELLED) archive_app__show_error(picked.name, result);
        (void)input__flush();
    }

    memory__free(entries.entries);
    return BRUCE_OK;
}
