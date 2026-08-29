#include "filemanager_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/paste.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#define FILEMANAGER_PREVIEW_MAX 4096

static bool filemanager__resume_after_handoff(void) {
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

static const char *filemanager__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void filemanager__parent_path(const char *path, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == NULL || slash == out) {
        snprintf(out, out_size, "/");
    } else {
        *slash = '\0';
    }
}

static bruce_result_t filemanager__new_entry(const char *directory, bool folder) {
    char name[BRUCE_STORAGE_NAME_MAX];
    bruce_result_t result = dialog__text_input(
        folder ? "New folder" : "New file", "Name", "", false, name, sizeof(name)
    );
    if (result != BRUCE_OK) return result;
    if (name[0] == '\0' || strchr(name, '/') != NULL || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char new_path[BRUCE_STORAGE_PATH_MAX];
    int written;
    if (strcmp(directory, "/") == 0) {
        written = snprintf(new_path, sizeof(new_path), "/%s", name);
    } else {
        written = snprintf(new_path, sizeof(new_path), "%s/%s", directory, name);
    }
    if (written < 0 || (size_t)written >= sizeof(new_path)) return BRUCE_ERR_RESOURCE_LIMIT;
    if (folder) return storage__mkdir(new_path);

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    result = storage__open(
        new_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result == BRUCE_OK) result = storage__close(file);
    return result;
}

static const char *filemanager__extension(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot != NULL ? dot : "";
}

static bool filemanager__is_editable_text(const char *path) {
    const char *dot = filemanager__extension(path);
    return dot[0] != '\0' &&
           (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".json") == 0 || strcasecmp(dot, ".conf") == 0);
}

static bool filemanager__is_gui_executable(const char *path) {
    const char *extension = filemanager__extension(path);
    return strcasecmp(extension, ".wasm") == 0 || strcasecmp(extension, ".elf") == 0;
}

static bool filemanager__escape_arg(const char *path, char *out, size_t out_size) {
    size_t written = 0;
    for (size_t i = 0; path[i] != '\0'; ++i) {
        if (path[i] == ' ' || path[i] == '\t' || path[i] == '\\' || path[i] == '\'' || path[i] == '"') {
            if (written + 1u >= out_size) return false;
            out[written++] = '\\';
        }
        if (written + 1u >= out_size) return false;
        out[written++] = path[i];
    }
    if (written + 1u > out_size) return false;
    out[written] = '\0';
    return true;
}

static bruce_result_t
filemanager__run_named_app(const char *app, const char *path, bool gui, bool read_only) {
    char escaped_path[BRUCE_STORAGE_PATH_MAX * 2 + 8];
    char args[BRUCE_STORAGE_PATH_MAX * 2 + 16];
    const char *arg_string = NULL;

    if (!filemanager__escape_arg(path, escaped_path, sizeof(escaped_path))) return BRUCE_ERR_INVALID_PATH;
    if (read_only) {
        int written = snprintf(args, sizeof(args), "-r %s", escaped_path);
        if (written < 0 || (size_t)written >= sizeof(args)) return BRUCE_ERR_RESOURCE_LIMIT;
        arg_string = args;
    } else {
        arg_string = escaped_path;
    }

    const bruce_environment_variable_t gui_env[] = {
        {.name = "GUI", .value = "1"}
    };
    int process = app_runner__run_with_environment(
        app, arg_string, BRUCE_LAUNCH_FOREGROUND, gui ? gui_env : NULL, gui ? 1u : 0u
    );
    if (process <= 0) return (bruce_result_t)process;
    return process__wait((bruce_process_id_t)process, UINT32_MAX);
}

static bruce_result_t filemanager__open_default(const char *path, bool gui) {
    gui = gui || filemanager__is_gui_executable(path);
    const bruce_environment_variable_t gui_env[] = {
        {.name = "GUI", .value = "1"}
    };
    int process = app_runner__run_path_with_environment(
        path, NULL, BRUCE_LAUNCH_FOREGROUND, gui ? gui_env : NULL, gui ? 1u : 0u
    );
    if (process <= 0) return (bruce_result_t)process;
    return process__wait((bruce_process_id_t)process, UINT32_MAX);
}

static bruce_result_t filemanager__pick_open_with_app(const char *path, bool gui) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Text",       .value = "text"  },
        {.label = "Image",      .value = "image" },
        {.label = "Wasm",       .value = "wasm"  },
        {.label = "ELF",        .value = "elf"   },
        {.label = "JavaScript", .value = "js"    },
        {.label = "Cancel",     .value = "cancel"},
    };
    /* Heap, not a local: sized off BRUCE_STORAGE_PATH_MAX, not
     * BRUCE_STORAGE_NAME_MAX, since `path` (and thus
     * filemanager__basename(path), when it has no '/') is only ever as long
     * as a full path but the compiler can't see that bound through the
     * pointer - a NAME_MAX-sized buffer would warn (-Werror=format-truncation)
     * on the snprintf below. That makes it too big to keep piling onto the
     * caller's task stack on top of dialog__choice()'s own frame below. */
    char *title = memory__malloc(16 + BRUCE_STORAGE_PATH_MAX);
    if (title == NULL) return BRUCE_ERR_NO_MEMORY;
    snprintf(title, 16 + BRUCE_STORAGE_PATH_MAX, "Open with %s", filemanager__basename(path));

    size_t selected = 0;
    bruce_result_t result =
        dialog__choice(title, NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected);
    memory__free(title);
    if (result != BRUCE_OK || strcmp(choices[selected].value, "cancel") == 0)
        return result == BRUCE_OK ? BRUCE_ERR_CANCELLED : result;

    const char *app = choices[selected].value;
    if (strcmp(app, "text") == 0) return filemanager__run_named_app("text", path, gui, false);
    if (strcmp(app, "image") == 0) return filemanager__run_named_app("image", path, gui, false);
    if (strcmp(app, "wasm") == 0) return filemanager__run_named_app("wasm", path, true, false);
    if (strcmp(app, "elf") == 0) return filemanager__run_named_app("elf", path, true, false);
    if (strcmp(app, "js") == 0) return filemanager__run_named_app("js", path, gui, false);
    return BRUCE_ERR_NOT_FOUND;
}

static bruce_result_t filemanager__read_preview(const char *path, char **out_text, bool *out_truncated) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t file_size = 0;
    result = storage__seek(file, 0, SEEK_END, &file_size);
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);
    if (result != BRUCE_OK) {
        (void)storage__close(file);
        return result;
    }

    size_t preview_size = file_size < FILEMANAGER_PREVIEW_MAX ? (size_t)file_size : FILEMANAGER_PREVIEW_MAX;
    char *text = memory__malloc(preview_size + 1u);
    if (text == NULL) {
        (void)storage__close(file);
        return BRUCE_ERR_NO_MEMORY;
    }

    size_t read_size = 0;
    result = storage__read(file, text, preview_size, &read_size);
    if (result == BRUCE_OK) {
        char extra;
        size_t extra_size = 0;
        result = storage__read(file, &extra, 1, &extra_size);
        *out_truncated = result == BRUCE_OK && extra_size > 0;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        memory__free(text);
        return result;
    }

    for (size_t i = 0; i < read_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\0' || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) text[i] = '.';
    }
    text[read_size] = '\0';
    *out_text = text;
    return BRUCE_OK;
}

static bruce_result_t filemanager__view_file(const char *path, bool gui) {
    char *text = NULL;
    bool truncated = false;
    bruce_result_t result = filemanager__read_preview(path, &text, &truncated);
    if (result != BRUCE_OK) return result;

    if (!gui) {
        stdio__printf("%s%s\n", text, truncated ? "\n[preview truncated]" : "");
        memory__free(text);
        return BRUCE_OK;
    }

    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    result = dialog__create_text_viewer(filemanager__basename(path), text, &viewer);
    memory__free(text);
    if (result != BRUCE_OK) return result;

    (void)input__flush();
    int text_size = 1;
    for (;;) {
        bruce_input_event_t event;
        result = input__read(&event, 100);
        if (result == BRUCE_ERR_NOT_FOREGROUND && filemanager__resume_after_handoff()) continue;
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

static bruce_result_t filemanager__edit_file(const char *path, bool gui) {
    if (!filemanager__is_editable_text(path)) return BRUCE_ERR_INVALID_ARGUMENT;
    return filemanager__run_named_app("text", path, gui, false);
}

static bruce_result_t filemanager__show_info(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    (void)storage__close(file);
    if (result != BRUCE_OK) return result;

    char message[BRUCE_STORAGE_PATH_MAX + 48];
    snprintf(message, sizeof(message), "%s\n%llu bytes", path, (unsigned long long)size);
    return dialog__message(BRUCE_DIALOG_INFO, "File info", message);
}

static void filemanager__show_error(const char *action, bruce_result_t result) {
    char message[160];
    ext_mem_loader__format_error_message(action, result, message, sizeof(message));
    (void)dialog__message(BRUCE_DIALOG_ERROR, "Apps", message);
}

/* "Copy" action: adds `path` (a file or folder) to the shared clipboard so a
 * later "Paste" here, in a different directory, or in another app entirely,
 * can paste it - see core_sdk/paste.h. */
static bruce_result_t filemanager__copy_entry(const char *path) {
    const char *source_paths[] = {path};
    return paste__set_files(source_paths, 1, BRUCE_PASTE_FILE_COPY);
}

/* "Paste" action: pastes the clipboard's file(s)/folder(s) into `directory`,
 * or reports BRUCE_ERR_INVALID_STATE if the clipboard doesn't hold files
 * (e.g. it's empty, or holds text copied by some other app). */
static bruce_result_t filemanager__paste_here(const char *directory) {
    if (paste__kind() != BRUCE_PASTE_FILES) return BRUCE_ERR_INVALID_STATE;
    return paste__paste_files(directory);
}

/* `kind` ("file"/"folder") only changes the confirmation dialog's wording;
 * storage__remove() itself already refuses a non-empty directory, so a
 * folder holding anything comes back as an ordinary error here rather than
 * being silently skipped. */
static bruce_result_t filemanager__delete_entry(const char *path, const char *kind) {
    const bruce_dialog_choice_t confirm_actions[] = {
        {.label = "Delete", .value = "delete"},
        {.label = "Cancel", .value = "cancel"},
    };
    char title[24];
    snprintf(title, sizeof(title), "Delete %s?", kind);

    size_t selected = 0;
    bruce_result_t result = dialog__choice(
        title, path, confirm_actions, sizeof(confirm_actions) / sizeof(confirm_actions[0]), &selected
    );
    if (result != BRUCE_OK) return result;
    if (strcmp(confirm_actions[selected].value, "delete") != 0) return BRUCE_ERR_CANCELLED;

    return storage__remove(path);
}

/* Prompts for a new name and renames `path` (file or folder) in place,
 * leaving it untouched on cancel/error. On success, `path` itself is
 * rewritten to the new full path so the caller can re-select it. */
static bruce_result_t filemanager__rename_entry(char *path, size_t path_size) {
    const char *base = filemanager__basename(path);
    char new_name[BRUCE_STORAGE_NAME_MAX];
    bruce_result_t result = dialog__text_input("Rename", base, base, false, new_name, sizeof(new_name));
    if (result != BRUCE_OK) return result;
    if (new_name[0] == '\0' || strchr(new_name, '/') != NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (strcmp(new_name, base) == 0) return BRUCE_OK;

    size_t dir_len = (size_t)(base - path);
    char new_path[BRUCE_STORAGE_PATH_MAX];
    if (dir_len >= sizeof(new_path)) return BRUCE_ERR_INVALID_PATH;
    memcpy(new_path, path, dir_len);
    int written = snprintf(new_path + dir_len, sizeof(new_path) - dir_len, "%s", new_name);
    if (written < 0 || (size_t)written >= sizeof(new_path) - dir_len) return BRUCE_ERR_RESOURCE_LIMIT;

    result = storage__rename(path, new_path);
    if (result == BRUCE_OK) snprintf(path, path_size, "%s", new_path);
    return result;
}

/* Folder counterpart to filemanager__show_info(): a directory can't be
 * storage__open()'d for a byte size, so this reports its entry count
 * instead. */
static bruce_result_t filemanager__show_folder_info(const char *path) {
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) return result;

    char message[BRUCE_STORAGE_PATH_MAX + 32];
    snprintf(message, sizeof(message), "%s\n%zu item%s", path, count, count == 1 ? "" : "s");
    return dialog__message(BRUCE_DIALOG_INFO, "Folder info", message);
}

int filemanager_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        stdio__printf("Browse and manage files.\n");
        return BRUCE_OK;
    }

    bool gui = runtime__gui_requested();
    const bruce_dialog_choice_t actions[] = {
        {.label = "Open",         .value = "open"  },
        {.label = "Open with...", .value = "openw" },
        {.label = "View",         .value = "view"  },
        {.label = "Edit",         .value = "edit"  },
        {.label = "Copy",         .value = "copy"  },
        {.label = "Rename",       .value = "rename"},
        {.label = "File info",    .value = "info"  },
        {.label = "Delete",       .value = "delete"},
        {.label = "Back",         .value = "back"  },
    };
    /* Long-pressing a folder row returns it here instead of descending into
     * it (see dialog__pick_file_ex()'s doc comment), so it gets this menu
     * instead of the file one above. */
    const bruce_dialog_choice_t folder_actions[] = {
        {.label = "Copy",        .value = "copy"  },
        {.label = "Rename",      .value = "rename"},
        {.label = "Folder info", .value = "info"  },
        {.label = "Delete",      .value = "delete"},
        {.label = "Back",        .value = "back"  },
    };
    /* Long-pressing the ".." row returns the directory being browsed itself
     * (see dialog__pick_file_ex()'s doc comment), so this menu acts on the
     * directory rather than an entry within it. */
    const bruce_dialog_choice_t directory_actions[] = {
        {.label = "New file",   .value = "new_file"  },
        {.label = "New folder", .value = "new_folder"},
        {.label = "Paste",      .value = "paste"     },
        {.label = "Back",       .value = "back"      },
    };
    (void)argc;
    (void)argv;

    bruce_dialog_render_params_t action_params = dialog__default_render_params(2);
    /* Lets a long press on a folder row return it below instead of
     * descending into it; out_long_press is left NULL since which entry
     * came back (probed via storage__list() further down) is all this loop
     * needs to know. */
    action_params.long_press_enabled = true;
    bool parent_entry = false;
    action_params.out_parent_entry = &parent_entry;
    /* The last file/folder picked, re-passed as dialog__pick_file_ex()'s
     * starting point below: it browses that entry's directory with the
     * entry itself pre-selected, so Esc/"Back" out of the action menu lands
     * the browser back on the same entry instead of just the same
     * directory. */
    char last_path[BRUCE_STORAGE_PATH_MAX] = "/";

    for (;;) {
        char path[BRUCE_STORAGE_PATH_MAX];
        bruce_result_t result =
            dialog__pick_file_ex(last_path, NULL, path, sizeof(path), "Filemanager", &action_params);
        if (result == BRUCE_ERR_CANCELLED && filemanager__resume_after_handoff()) {
            (void)input__flush();
            continue;
        }
        if (result == BRUCE_ERR_CANCELLED) return 0;
        if (result != BRUCE_OK) {
            filemanager__show_error("Browse", result);
            return result;
        }
        snprintf(last_path, sizeof(last_path), "%s", path);

        /* A short press always yields a file, same as before this feature
         * existed; a long press yields a folder only when it landed on one
         * (see dialog__pick_file_ex()'s doc comment) - it still returns the
         * file itself, same as a short press, when it landed on a file. So
         * rather than track which kind of press this was, just check what
         * came back: storage__list() succeeds on a directory and fails on a
         * file. */
        size_t probe_count = 0;
        bool is_folder = storage__list(path, NULL, 0, &probe_count) == BRUCE_OK;

        size_t selected = 0;
        /* Plain dialog__choice(), not the picker's full-bleed action_params:
         * this is a small action menu over the already-visible file browser,
         * so it reads better as a popup window than another full screen. */
        const bruce_dialog_choice_t *menu =
            parent_entry ? directory_actions : (is_folder ? folder_actions : actions);
        size_t menu_count = parent_entry ? sizeof(directory_actions) / sizeof(directory_actions[0])
                            : is_folder   ? sizeof(folder_actions) / sizeof(folder_actions[0])
                                          : sizeof(actions) / sizeof(actions[0]);
        result = dialog__choice(path, NULL, menu, menu_count, &selected);
        if (result == BRUCE_ERR_CANCELLED && filemanager__resume_after_handoff()) {
            (void)input__flush();
            continue;
        }
        if (result == BRUCE_ERR_CANCELLED) {
            if (!parent_entry && is_folder) {
                filemanager__parent_path(path, last_path, sizeof(last_path));
            }
            (void)input__flush();
            continue;
        }
        if (result != BRUCE_OK) {
            filemanager__show_error("Action", result);
            continue;
        }

        const char *action = menu[selected].value;
        if (strcmp(action, "back") == 0) {
            if (!parent_entry && is_folder) {
                filemanager__parent_path(path, last_path, sizeof(last_path));
            }
            (void)input__flush();
            continue;
        }
        if (parent_entry) {
            if (strcmp(action, "new_file") == 0) {
                result = filemanager__new_entry(path, false);
            } else if (strcmp(action, "new_folder") == 0) {
                result = filemanager__new_entry(path, true);
            } else if (strcmp(action, "paste") == 0) {
                result = filemanager__paste_here(path);
            }
        } else if (is_folder) {
            if (strcmp(action, "copy") == 0) {
                result = filemanager__copy_entry(path);
            } else if (strcmp(action, "rename") == 0) {
                result = filemanager__rename_entry(path, sizeof(path));
                if (result == BRUCE_OK) snprintf(last_path, sizeof(last_path), "%s", path);
            } else if (strcmp(action, "info") == 0) {
                result = filemanager__show_folder_info(path);
            } else if (strcmp(action, "delete") == 0) {
                result = filemanager__delete_entry(path, "folder");
            }
        } else if (strcmp(action, "open") == 0) {
            result = filemanager__open_default(path, gui);
        } else if (strcmp(action, "openw") == 0) {
            result = filemanager__pick_open_with_app(path, gui);
        } else if (strcmp(action, "view") == 0) {
            result = filemanager__view_file(path, gui);
        } else if (strcmp(action, "edit") == 0) {
            result = filemanager__edit_file(path, gui);
        } else if (strcmp(action, "copy") == 0) {
            result = filemanager__copy_entry(path);
        } else if (strcmp(action, "rename") == 0) {
            result = filemanager__rename_entry(path, sizeof(path));
            if (result == BRUCE_OK) snprintf(last_path, sizeof(last_path), "%s", path);
        } else if (strcmp(action, "info") == 0) {
            result = filemanager__show_info(path);
        } else if (strcmp(action, "delete") == 0) {
            result = filemanager__delete_entry(path, "file");
        }
        if (result != BRUCE_OK && result != BRUCE_ERR_CANCELLED) {
            filemanager__show_error(menu[selected].label, result);
        }
        (void)input__flush();
    }
}
