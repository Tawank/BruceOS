#include "filemanager_actions.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/environment.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/filetype.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#include "filemanager_internal.h"
#include "filemanager_network.h"
#include "filemanager_network_internal.h"

#define FILEMANAGER_PREVIEW_MAX 4096

bruce_result_t filemanager__new_entry(const char *directory, bool folder) {
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

bruce_result_t filemanager__open_default(const char *path, bool gui) {
    /* A "/Network"-discovered location (see filemanager_network.h) is named
     * "<label>.<provider name>" -- a real extension, but resolved here
     * straight from "/config/filemanager.conf" (the same lookup that
     * produced the entry in the first place) rather than through
     * filetype__identify() below, so a provider still works with no
     * matching "/config/extensions.conf" entry -- one just buys it a proper
     * icon in the listing (see filemanager_network.c's top comment). */
    char provider_program[FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];
    if (filemanager_network__resolve_program(path, provider_program, sizeof(provider_program))) {
        return filemanager__run_named_app(provider_program, path, gui, false);
    }

    /* Shell scripts loaded the plain way (the ".sh" -> "shell" loader used by
     * the shell itself, see app_runner__register_loader() in main.c) run
     * headless with no visible output. Opening one from the file manager
     * instead launches a terminal and types its path into that terminal's
     * own interactive shell, the same as running it from any other shell
     * session -- so it's visible, and stays the *only* place ".sh" maps to
     * "terminal"; running a script from inside an existing terminal must
     * keep going through the plain "shell" loader, not recurse into a
     * nested terminal. */
    /* One filetype lookup covers both checks below - it also catches an
     * extensionless script via its shebang, so it opens the same way its
     * ".sh" equivalent would. */
    bruce_filetype_info_t info;
    bool identified = filetype__identify(path, &info) == BRUCE_OK && !info.is_directory;
    if (identified && strcmp(info.program, "shell") == 0) {
        return filemanager__run_named_app("terminal", path, true, false);
    }
    gui = gui || (identified && (strcmp(info.program, "wasm") == 0 || strcmp(info.program, "elf") == 0));
    const bruce_environment_variable_t gui_env[] = {
        {.name = "GUI", .value = "1"}
    };
    int process = app_runner__run_path_with_environment(
        path, NULL, BRUCE_LAUNCH_FOREGROUND, gui ? gui_env : NULL, gui ? 1u : 0u
    );
    return filemanager__wait_child(process);
}

#define FILEMANAGER_OPEN_WITH_APPS_MAX 12

static bruce_result_t filemanager__run_terminal_command(const char *command_line) {
    const bruce_environment_variable_t gui_env[] = {
        {.name = "GUI", .value = "1"}
    };
    int process = app_runner__run_with_environment("terminal", command_line, BRUCE_LAUNCH_FOREGROUND, gui_env, 1u);
    return filemanager__wait_child(process);
}

static bool filemanager__strip_app_extension(const char *filename, char *out_name, size_t out_size) {
    static const char *const extensions[] = {".elf", ".wasm", ".js"};
    size_t name_length = strlen(filename);
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        size_t extension_length = strlen(extensions[i]);
        if (name_length > extension_length &&
            strcasecmp(filename + name_length - extension_length, extensions[i]) == 0) {
            size_t base_length = name_length - extension_length;
            if (base_length >= out_size) return false;
            memcpy(out_name, filename, base_length);
            out_name[base_length] = '\0';
            return true;
        }
    }
    return false;
}

static size_t filemanager__list_installed_apps(char names[][BRUCE_STORAGE_NAME_MAX], size_t capacity) {
    size_t total = 0;
    if (storage__list("/apps", NULL, 0, &total) != BRUCE_OK || total == 0) return 0;
    bruce_storage_entry_t *entries = memory__calloc(total, sizeof(*entries));
    if (entries == NULL) return 0;
    size_t count = 0;
    if (storage__list("/apps", entries, total, &total) == BRUCE_OK) {
        for (size_t i = 0; i < total && count < capacity; ++i) {
            if (entries[i].type != BRUCE_STORAGE_ENTRY_FILE) continue;
            if (filemanager__strip_app_extension(entries[i].name, names[count], BRUCE_STORAGE_NAME_MAX)) count++;
        }
    }
    memory__free(entries);
    return count;
}

bruce_result_t filemanager__pick_open_with_app(const char *path, bool gui) {
    char app_names[FILEMANAGER_OPEN_WITH_APPS_MAX][BRUCE_STORAGE_NAME_MAX];
    size_t app_count = filemanager__list_installed_apps(app_names, FILEMANAGER_OPEN_WITH_APPS_MAX);

    size_t capacity = 8u + app_count;
    bruce_dialog_choice_t *choices = memory__malloc(capacity * sizeof(*choices));
    if (choices == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t n = 0;
    choices[n++] = (bruce_dialog_choice_t){.label = "Text", .value = "text"};
    choices[n++] = (bruce_dialog_choice_t){.label = "xxd (hex dump)", .value = "xxd"};
    size_t apps_start = n;
    for (size_t i = 0; i < app_count; ++i) choices[n++] = (bruce_dialog_choice_t){.label = app_names[i], .value = app_names[i]};
    size_t apps_end = n;
    choices[n++] = (bruce_dialog_choice_t){.label = "Custom command...", .value = "custom"};
    choices[n++] = (bruce_dialog_choice_t){.label = "Cancel", .value = "cancel"};

    char *title = memory__malloc(16 + BRUCE_STORAGE_PATH_MAX);
    if (title == NULL) {
        memory__free(choices);
        return BRUCE_ERR_NO_MEMORY;
    }
    snprintf(title, 16 + BRUCE_STORAGE_PATH_MAX, "Open with %s", filemanager__basename(path));

    size_t selected = 0;
    bruce_result_t result = dialog__choice(title, NULL, choices, n, &selected);
    memory__free(title);
    if (result != BRUCE_OK || strcmp(choices[selected].value, "cancel") == 0) {
        memory__free(choices);
        return result == BRUCE_OK ? BRUCE_ERR_CANCELLED : result;
    }

    if (selected >= apps_start && selected < apps_end) {
        char app_name[BRUCE_STORAGE_NAME_MAX];
        snprintf(app_name, sizeof(app_name), "%s", choices[selected].value);
        memory__free(choices);
        return filemanager__run_named_app(app_name, path, true, false);
    }

    char app[16];
    snprintf(app, sizeof(app), "%s", choices[selected].value);
    memory__free(choices);

    if (strcmp(app, "text") == 0) return filemanager__run_named_app("text", path, gui, false);

    char escaped_path[BRUCE_STORAGE_PATH_MAX * 2 + 8];
    if (!filemanager__escape_arg(path, escaped_path, sizeof(escaped_path))) return BRUCE_ERR_INVALID_PATH;

    if (strcmp(app, "xxd") == 0) {
        char command[sizeof(escaped_path) + 8];
        int written = snprintf(command, sizeof(command), "xxd %s", escaped_path);
        if (written < 0 || (size_t)written >= sizeof(command)) return BRUCE_ERR_RESOURCE_LIMIT;
        return filemanager__run_terminal_command(command);
    }
    if (strcmp(app, "custom") == 0) {
        /* Pre-filled with the (escaped) path rather than empty, so the user
         * edits it into a full command (e.g. "xxd -l 64 <path>", "cat <path>
         * | grep foo") instead of typing the path themselves. */
        char command[BRUCE_STORAGE_PATH_MAX * 2 + 64];
        bruce_result_t text_result =
            dialog__text_input("Custom command", "Command", escaped_path, false, command, sizeof(command));
        if (text_result != BRUCE_OK) return text_result;
        if (command[0] == '\0') return BRUCE_ERR_CANCELLED;
        return filemanager__run_terminal_command(command);
    }
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

bruce_result_t filemanager__view_file(const char *path, bool gui) {
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
        if (result == BRUCE_ERR_NOT_FOREGROUND && filemanager__resume_after_handoff()) {
            /* Whatever painted over the screen while backgrounded isn't
             * restored automatically on switching back (see
             * image_loader_app.c's identical fix) -- dialog__viewer_scroll()
             * always repaints from scratch even for a zero-length scroll, so
             * this is the viewer framework's own way to force that redraw. */
            (void)dialog__viewer_scroll(viewer, 0);
            continue;
        }
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

bruce_result_t filemanager__edit_file(const char *path, bool gui) {
    return filemanager__run_named_app("text", path, gui, false);
}

bruce_result_t filemanager__show_info(const char *path) {
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

/* Folder counterpart to filemanager__show_info(): a directory can't be
 * storage__open()'d for a byte size, so this reports its entry count
 * instead. */
bruce_result_t filemanager__show_folder_info(const char *path) {
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) return result;

    char message[BRUCE_STORAGE_PATH_MAX + 32];
    snprintf(message, sizeof(message), "%s\n%zu item%s", path, count, count == 1 ? "" : "s");
    return dialog__message(BRUCE_DIALOG_INFO, "Folder info", message);
}

/* Prompts for a new name and renames `path` (file or folder) in place,
 * leaving it untouched on cancel/error. On success, `path` itself is
 * rewritten to the new full path so the caller can re-select it. */
bruce_result_t filemanager__rename_entry(char *path, size_t path_size) {
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

/* `kind` ("file"/"folder") only changes the confirmation dialog's wording;
 * storage__remove() itself already refuses a non-empty directory, so a
 * folder holding anything comes back as an ordinary error here rather than
 * being silently skipped. */
bruce_result_t filemanager__delete_entry(const char *path, const char *kind) {
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
