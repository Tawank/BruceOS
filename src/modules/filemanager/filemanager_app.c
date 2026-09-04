#include "filemanager_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/filetype.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/clipboard.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#include "filemanager_network_internal.h"

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
 * can paste it - see core_sdk/clipboard.h. */
static bruce_result_t filemanager__copy_entry(const char *path) {
    const char *source_paths[] = {path};
    return clipboard__set_files(source_paths, 1, BRUCE_CLIPBOARD_FILE_COPY);
}

/* Binary-clipboard half of filemanager__paste_here(): writes the clipboard's
 * raw bytes into `directory` under its own suggested name
 * (clipboard__binary_filename()) when it has one, or otherwise prompts for
 * one -- some copier (e.g. a shell `wl-copy` piped from stdin) has no
 * filename of its own to offer. */
static bruce_result_t filemanager__paste_binary_here(const char *directory) {
    const char *suggested_name = clipboard__binary_filename();
    char name[BRUCE_STORAGE_NAME_MAX];
    if (suggested_name != NULL) {
        snprintf(name, sizeof(name), "%s", suggested_name);
    } else {
        bruce_result_t result = dialog__text_input("Paste", "File name", "", false, name, sizeof(name));
        if (result != BRUCE_OK) return result;
    }
    if (name[0] == '\0' || strchr(name, '/') != NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char target_path[BRUCE_STORAGE_PATH_MAX];
    int written = strcmp(directory, "/") == 0 ? snprintf(target_path, sizeof(target_path), "/%s", name)
                                               : snprintf(target_path, sizeof(target_path), "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= sizeof(target_path)) return BRUCE_ERR_RESOURCE_LIMIT;
    return clipboard__paste_binary(target_path);
}

/* Builds `directory/base_name`, or, if that already exists, the lowest-
 * numbered `directory/base(N).ext` (N >= 1) that doesn't - used when pasting
 * a copy back into the very directory its source already lives in, where
 * colliding with the original just means "make this copy its own name"
 * rather than a real conflict to ask the user about. */
static bruce_result_t
filemanager__unique_destination(const char *directory, const char *base_name, char *out, size_t out_size) {
    int written = strcmp(directory, "/") == 0 ? snprintf(out, out_size, "/%s", base_name)
                                               : snprintf(out, out_size, "%s/%s", directory, base_name);
    if (written < 0 || (size_t)written >= out_size) return BRUCE_ERR_RESOURCE_LIMIT;

    bool exists = false;
    bruce_result_t result = storage__exists(out, &exists);
    if (result != BRUCE_OK || !exists) return result;

    /* A leading dot (e.g. ".bashrc") marks a hidden file, not an extension -
     * keep it as part of the stem instead of splitting on it. */
    const char *dot = strrchr(base_name, '.');
    size_t stem_length = dot != NULL && dot != base_name ? (size_t)(dot - base_name) : strlen(base_name);
    const char *extension = dot != NULL && dot != base_name ? dot : "";

    for (int n = 1; n <= 999; ++n) {
        char candidate[BRUCE_STORAGE_NAME_MAX];
        int candidate_written =
            snprintf(candidate, sizeof(candidate), "%.*s(%d)%s", (int)stem_length, base_name, n, extension);
        if (candidate_written < 0 || (size_t)candidate_written >= sizeof(candidate)) {
            return BRUCE_ERR_RESOURCE_LIMIT;
        }

        written = strcmp(directory, "/") == 0 ? snprintf(out, out_size, "/%s", candidate)
                                               : snprintf(out, out_size, "%s/%s", directory, candidate);
        if (written < 0 || (size_t)written >= out_size) return BRUCE_ERR_RESOURCE_LIMIT;

        result = storage__exists(out, &exists);
        if (result != BRUCE_OK || !exists) return result;
    }
    return BRUCE_ERR_ALREADY_EXISTS;
}

/* Asks whether to replace the file/folder already at `path`. Returns
 * BRUCE_ERR_CANCELLED if the user declines, so the caller can bail out the
 * same way it would for any other cancelled prompt. */
static bruce_result_t filemanager__confirm_overwrite(const char *path) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Overwrite", .value = "overwrite"},
        {.label = "Cancel",    .value = "cancel"   },
    };
    size_t selected = 0;
    bruce_result_t result = dialog__choice(
        "Replace existing item?", path, choices, sizeof(choices) / sizeof(choices[0]), &selected
    );
    if (result != BRUCE_OK) return result;
    return strcmp(choices[selected].value, "overwrite") == 0 ? BRUCE_OK : BRUCE_ERR_CANCELLED;
}

/* Files/folders half of filemanager__paste_here(): pastes every clipboard
 * entry into `directory`, resolving name conflicts instead of just failing
 * with BRUCE_ERR_ALREADY_EXISTS the way the plain clipboard__paste_files()
 * does - pasting back into an entry's own directory gets it a fresh
 * "name(1).ext"-style name automatically, while colliding with some other,
 * unrelated file asks before overwriting it. */
static bruce_result_t filemanager__paste_files_here(const char *directory) {
    size_t count = clipboard__file_count();
    if (count == 0) return BRUCE_ERR_INVALID_STATE;

    for (size_t i = 0; i < count; ++i) {
        const char *source = clipboard__get_file(i);
        if (source == NULL) return BRUCE_ERR_INVALID_STATE;
        /* Copied out before any dialog below runs, since a slow prompt could
         * otherwise span a clipboard__set_files() from elsewhere that frees
         * the borrowed pointer clipboard__get_file() just handed back. */
        char source_copy[BRUCE_STORAGE_PATH_MAX];
        snprintf(source_copy, sizeof(source_copy), "%s", source);
        const char *base_name = filemanager__basename(source_copy);

        char destination[BRUCE_STORAGE_PATH_MAX];
        int written;
        if (strcmp(directory, "/") == 0) {
            written = snprintf(destination, sizeof(destination), "/%s", base_name);
        } else {
            written = snprintf(destination, sizeof(destination), "%s/%s", directory, base_name);
        }
        if (written < 0 || (size_t)written >= sizeof(destination)) return BRUCE_ERR_RESOURCE_LIMIT;

        bool overwrite = false;
        if (strcmp(destination, source_copy) == 0) {
            bruce_result_t result =
                filemanager__unique_destination(directory, base_name, destination, sizeof(destination));
            if (result != BRUCE_OK) return result;
        } else {
            bool exists = false;
            bruce_result_t result = storage__exists(destination, &exists);
            if (result != BRUCE_OK) return result;
            if (exists) {
                result = filemanager__confirm_overwrite(destination);
                if (result != BRUCE_OK) return result;
                overwrite = true;
            }
        }

        bruce_result_t result = clipboard__paste_file_as(i, destination, overwrite);
        if (result != BRUCE_OK) return result;
    }
    return BRUCE_OK;
}

/* "Paste" action: pastes the clipboard's file(s)/folder(s), or binary
 * payload, into `directory`; reports BRUCE_ERR_INVALID_STATE if the
 * clipboard holds neither (e.g. it's empty, or holds text copied by some
 * other app). */
static bruce_result_t filemanager__paste_here(const char *directory) {
    bruce_clipboard_kind_t kind = clipboard__kind();
    if (kind == BRUCE_CLIPBOARD_FILES) return filemanager__paste_files_here(directory);
    if (kind == BRUCE_CLIPBOARD_BINARY) return filemanager__paste_binary_here(directory);
    return BRUCE_ERR_INVALID_STATE;
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

/**
 * @name Network folder
 *
 * "/Network" is a plain directory, populated on entry with one plain file
 * per remote location a *provider* command discovers -- filemanager itself
 * has no idea what SSH or SFTP are. A provider is any command listed in
 * "/config/network_providers.conf" (one name per line, "#" comments,
 * defaulted to just "sftp" the first time this runs) that understands
 * `<command> list --autodiscover`: run with no GUI and its stdout captured,
 * it prints one "<display name>\t<location>" line per location it knows
 * about (e.g. host aliases read from "/.ssh/config", plus a "New
 * connection..." line of the provider's own choosing). Each line becomes
 * "/Network/<display name>.<provider>", containing just the location string
 * -- extensions.conf's normal program-by-extension dispatch (".sftp" ->
 * "sftp") is what makes opening that file launch the right app, so adding a
 * new location type later is a new provider command plus one line in the
 * config, with no filemanager code changes.
 * @{
 */

#define FILEMANAGER_NETWORK_DIR "/Network"
#define FILEMANAGER_NETWORK_PROVIDERS_CONF "/config/network_providers.conf"
#define FILEMANAGER_NETWORK_PROVIDERS_DEFAULT "sftp\n"
#define FILEMANAGER_NETWORK_PROVIDER_MAX 8
#define FILEMANAGER_NETWORK_CAPTURE_MAX 4096
/* FILEMANAGER_NETWORK_PROVIDER_NAME_MAX and the pure provider-line/
 * display-name parsing helpers now live in filemanager_network_internal.h --
 * selftest unit-tests those directly (see its header comment). */

void filemanager__network_parse_providers(
    char *text, char providers[][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX], size_t max_providers, size_t *out_count
) {
    *out_count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(text, "\n", &saveptr);
    while (line != NULL && *out_count < max_providers) {
        while (*line == ' ' || *line == '\t') ++line;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        if (len > 0 && len < FILEMANAGER_NETWORK_PROVIDER_NAME_MAX && line[0] != '#') {
            snprintf(providers[*out_count], FILEMANAGER_NETWORK_PROVIDER_NAME_MAX, "%s", line);
            ++*out_count;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static void filemanager__network_load_providers(
    char providers[][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX], size_t max_providers, size_t *out_count
) {
    *out_count = 0;
    bool exists = false;
    if (storage__exists(FILEMANAGER_NETWORK_PROVIDERS_CONF, &exists) != BRUCE_OK || !exists) {
        bruce_file_id_t seed = BRUCE_FILE_ID_INVALID;
        if (storage__open(
                FILEMANAGER_NETWORK_PROVIDERS_CONF,
                BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &seed
            ) == BRUCE_OK) {
            size_t written = 0;
            (void)storage__write(
                seed, FILEMANAGER_NETWORK_PROVIDERS_DEFAULT, strlen(FILEMANAGER_NETWORK_PROVIDERS_DEFAULT),
                &written
            );
            (void)storage__close(seed);
        }
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(FILEMANAGER_NETWORK_PROVIDERS_CONF, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) {
        if (max_providers > 0) {
            snprintf(providers[0], FILEMANAGER_NETWORK_PROVIDER_NAME_MAX, "sftp");
            *out_count = 1;
        }
        return;
    }
    char *text = memory__malloc(1024);
    if (text == NULL) {
        (void)storage__close(file);
        return;
    }
    size_t total = 0;
    for (;;) {
        size_t chunk = 0;
        if (storage__read(file, text + total, 1023 - total, &chunk) != BRUCE_OK || chunk == 0) break;
        total += chunk;
        if (total >= 1023) break;
    }
    (void)storage__close(file);
    text[total] = '\0';

    filemanager__network_parse_providers(text, providers, max_providers, out_count);
    memory__free(text);
}

/* Runs "<provider> list --autodiscover" and captures its stdout via a
 * redirected stdio session -- the same trick the shell uses for "$(...)"
 * command substitution (see shell_executor__capture_external() in
 * modules/shell/shell_executor.c), built entirely on public core_sdk
 * primitives so this needs no new core plumbing. Output beyond out_capacity
 * is silently dropped rather than growing the buffer: a location listing is
 * expected to be short text, not arbitrary command output. */
static bruce_result_t
filemanager__network_capture(const char *provider, char *out, size_t out_capacity, size_t *out_size) {
    *out_size = 0;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK) return BRUCE_ERR_IO;
    if (stdio__session_route_children(session) != BRUCE_OK) {
        (void)stdio__session_close(session);
        return BRUCE_ERR_IO;
    }
    int process = app_runner__run(provider, "list --autodiscover", BRUCE_LAUNCH_FOREGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (process <= 0) {
        (void)stdio__session_close(session);
        return BRUCE_ERR_NOT_FOUND;
    }

    size_t total = 0;
    bruce_process_status_t status = {0};
    bool complete = false;
    while (!complete) {
        char chunk[128];
        size_t size = 0;
        while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
            size_t copy = size < out_capacity - 1u - total ? size : out_capacity - 1u - total;
            if (copy > 0) memcpy(out + total, chunk, copy);
            total += copy;
        }
        bruce_result_t waited = process__wait_status((bruce_process_id_t)process, 0, &status);
        complete = waited == BRUCE_OK;
        if (!complete && waited != BRUCE_ERR_TIMEOUT) break;
        if (!complete) (void)runtime__delay(20);
    }
    char chunk[128];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        size_t copy = size < out_capacity - 1u - total ? size : out_capacity - 1u - total;
        if (copy > 0) memcpy(out + total, chunk, copy);
        total += copy;
    }
    (void)stdio__session_close(session);
    out[total] = '\0';
    *out_size = total;
    return BRUCE_OK;
}

void filemanager__network_sanitize_name(const char *name, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0' && j + 1 < out_size; ++i) {
        char c = name[i];
        out[j++] = (c == '/' || c == '\\') ? '_' : c;
    }
    out[j] = '\0';
}

static bruce_result_t filemanager__network_write_location(
    const char *provider, const char *display_name, const char *location
) {
    char safe_name[BRUCE_STORAGE_NAME_MAX];
    filemanager__network_sanitize_name(display_name, safe_name, sizeof(safe_name));
    if (safe_name[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;

    char path[BRUCE_STORAGE_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s.%s", FILEMANAGER_NETWORK_DIR, safe_name, provider);
    if (written < 0 || (size_t)written >= sizeof(path)) return BRUCE_ERR_RESOURCE_LIMIT;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) return result;
    size_t written_size = 0;
    result = storage__write(file, location, strlen(location), &written_size);
    (void)storage__close(file);
    return result;
}

/* Deletes every /Network entry that belongs to a known provider (matched by
 * extension) before repopulating, so a host removed from e.g. "/.ssh/config"
 * doesn't linger as a stale file forever. Anything with an unrecognized
 * extension (a user's own file dropped in there) is left alone. */
static void filemanager__network_clear_stale(
    char providers[][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX], size_t provider_count
) {
    bruce_storage_entry_t entries[32];
    size_t count = 0;
    if (storage__list(FILEMANAGER_NETWORK_DIR, entries, 32, &count) != BRUCE_OK) return;
    if (count > 32) count = 32;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].type != BRUCE_STORAGE_ENTRY_FILE) continue;
        const char *dot = strrchr(entries[i].name, '.');
        if (dot == NULL) continue;
        for (size_t p = 0; p < provider_count; ++p) {
            if (strcmp(dot + 1, providers[p]) == 0) {
                /* entries[i].name is a char[BRUCE_STORAGE_NAME_MAX] field, but GCC's
                 * -Werror=format-truncation doesn't infer that array's declared size just
                 * from the expression here, so it has to be spelled out via precision for
                 * the checker to see this can't overflow `path`. */
                char path[BRUCE_STORAGE_PATH_MAX];
                snprintf(
                    path, sizeof(path), "%s/%.*s", FILEMANAGER_NETWORK_DIR, BRUCE_STORAGE_NAME_MAX - 1,
                    entries[i].name
                );
                (void)storage__remove(path);
                break;
            }
        }
    }
}

/* Rebuilds "/Network" from every configured provider's autodiscover output.
 * Best-effort throughout: a provider that's missing, fails, or times out
 * just leaves that provider's locations absent rather than blocking entry
 * into the folder. */
static void filemanager__network_refresh(void) {
    bool exists = false;
    if (storage__exists(FILEMANAGER_NETWORK_DIR, &exists) != BRUCE_OK) return;
    if (!exists && storage__mkdir(FILEMANAGER_NETWORK_DIR) != BRUCE_OK) return;

    char providers[FILEMANAGER_NETWORK_PROVIDER_MAX][FILEMANAGER_NETWORK_PROVIDER_NAME_MAX];
    size_t provider_count = 0;
    filemanager__network_load_providers(providers, FILEMANAGER_NETWORK_PROVIDER_MAX, &provider_count);
    if (provider_count == 0) return;

    filemanager__network_clear_stale(providers, provider_count);

    char *capture = memory__malloc(FILEMANAGER_NETWORK_CAPTURE_MAX);
    if (capture == NULL) return;
    for (size_t p = 0; p < provider_count; ++p) {
        size_t capture_size = 0;
        if (filemanager__network_capture(
                providers[p], capture, FILEMANAGER_NETWORK_CAPTURE_MAX, &capture_size
            ) != BRUCE_OK) {
            continue;
        }
        char *saveptr = NULL;
        char *line = strtok_r(capture, "\n", &saveptr);
        while (line != NULL) {
            size_t len = strlen(line);
            while (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
            char *tab = strchr(line, '\t');
            if (tab != NULL) {
                *tab = '\0';
                const char *display_name = line;
                const char *location = tab + 1;
                if (display_name[0] != '\0') {
                    (void)filemanager__network_write_location(providers[p], display_name, location);
                }
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    memory__free(capture);
}

/** @} */

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
    /* Same as directory_actions, but for "/Network" specifically -- see the
     * "Network folder" section above. Discovery already runs once up front
     * (below), so this is a manual way to re-run it without leaving and
     * relaunching filemanager, e.g. right after editing "/.ssh/config". */
    const bruce_dialog_choice_t network_directory_actions[] = {
        {.label = "New file",          .value = "new_file"},
        {.label = "New folder",        .value = "new_folder"},
        {.label = "Paste",             .value = "paste"    },
        {.label = "Refresh locations", .value = "refresh"  },
        {.label = "Back",              .value = "back"     },
    };
    (void)argc;
    (void)argv;

    /* Best-effort: populates "/Network" from whatever providers are
     * configured before the browser ever shows it, so the first visit isn't
     * empty/stale. See the "Network folder" section above. */
    filemanager__network_refresh();

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

        /* A plain file (neither the ".." parent row nor a folder) gets its
         * extension's own extra actions (e.g. an archive's "Extract here",
         * see core_sdk/filetype.h's bruce_filetype_action_t) spliced into
         * `actions` just before "Back", so file_info must be identified up
         * here rather than lazily inside filemanager__open_default() -
         * dispatch below (the final `else` of the action chain) reads it
         * back by comparing the chosen value against file_info.actions[]. */
        bruce_filetype_info_t file_info;
        bool has_file_info = false;
        bruce_dialog_choice_t file_actions[sizeof(actions) / sizeof(actions[0]) + BRUCE_FILETYPE_MAX_ACTIONS];
        size_t file_actions_count = sizeof(actions) / sizeof(actions[0]);
        if (!parent_entry && !is_folder) {
            has_file_info = filetype__identify(path, &file_info) == BRUCE_OK && !file_info.is_directory;
            memcpy(file_actions, actions, sizeof(actions));
            if (has_file_info && file_info.action_count > 0) {
                /* Splice in before the last entry ("Back"). */
                file_actions[file_actions_count - 1 + file_info.action_count] = actions[file_actions_count - 1];
                for (size_t i = 0; i < file_info.action_count; ++i) {
                    file_actions[file_actions_count - 1 + i].label = file_info.actions[i].label;
                    file_actions[file_actions_count - 1 + i].value = file_info.actions[i].program;
                    file_actions[file_actions_count - 1 + i].icon_name = NULL;
                    file_actions[file_actions_count - 1 + i].right_text = NULL;
                }
                file_actions_count += file_info.action_count;
            }
        }

        bool is_network_dir = parent_entry && strcmp(path, FILEMANAGER_NETWORK_DIR) == 0;

        size_t selected = 0;
        /* Plain dialog__choice(), not the picker's full-bleed action_params:
         * this is a small action menu over the already-visible file browser,
         * so it reads better as a popup window than another full screen. */
        const bruce_dialog_choice_t *menu = parent_entry
                                                 ? (is_network_dir ? network_directory_actions : directory_actions)
                                             : is_folder ? folder_actions
                                                          : file_actions;
        size_t menu_count =
            parent_entry
                ? (is_network_dir ? sizeof(network_directory_actions) / sizeof(network_directory_actions[0])
                                   : sizeof(directory_actions) / sizeof(directory_actions[0]))
            : is_folder ? sizeof(folder_actions) / sizeof(folder_actions[0])
                        : file_actions_count;
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
            } else if (strcmp(action, "refresh") == 0) {
                filemanager__network_refresh();
                result = BRUCE_OK;
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
        } else {
            /* Not one of the fixed actions above - must be one of
             * file_info.actions[] spliced into the menu earlier. */
            result = BRUCE_ERR_NOT_FOUND;
            for (size_t i = 0; has_file_info && i < file_info.action_count; ++i) {
                if (strcmp(action, file_info.actions[i].program) == 0) {
                    result = filemanager__run_named_app(file_info.actions[i].program, path, gui, false);
                    break;
                }
            }
        }
        if (result != BRUCE_OK && result != BRUCE_ERR_CANCELLED) {
            filemanager__show_error(menu[selected].label, result);
        }
        (void)input__flush();
    }
}
