#include "filemanager_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/environment.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/filetype.h"
#include "core_sdk/input.h"
#include "core_sdk/launcher.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#include "filemanager_actions.h"
#include "filemanager_clipboard.h"
#include "filemanager_internal.h"
#include "filemanager_network.h"
#include "filemanager_pathicons.h"

/* filemanager_app_main()'s dispatch loop, plus the handful of small helpers
 * every other file of this module needs at least one of (see
 * filemanager_internal.h). The single-entry file/folder actions live in
 * filemanager_actions.c, Copy/Paste in filemanager_clipboard.c, and the
 * "/Network" folder feature in filemanager_network.c -- this file just wires
 * them together behind the action menus below. */

bool filemanager__resume_after_handoff(void) {
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

const char *filemanager__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

void filemanager__parent_path(const char *path, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == NULL || slash == out) {
        snprintf(out, out_size, "/");
    } else {
        *slash = '\0';
    }
}

bool filemanager__escape_arg(const char *path, char *out, size_t out_size) {
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

bruce_result_t
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

void filemanager__show_error(const char *action, bruce_result_t result) {
    char message[160];
    ext_mem_loader__format_error_message(action, result, message, sizeof(message));
    (void)dialog__message(BRUCE_DIALOG_ERROR, "Apps", message);
}

int filemanager_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        stdio__printf("Browse and manage files.\n");
        return BRUCE_OK;
    }

    bool gui = runtime__gui_requested();
    const bruce_dialog_choice_t actions[] = {
        {.label = "Open",         .value = "open"       },
        {.label = "Open with...", .value = "openw"      },
        {.label = "View",         .value = "view"       },
        {.label = "Edit",         .value = "edit"       },
        {.label = "Copy",         .value = "copy"       },
        {.label = "Rename",       .value = "rename"     },
        {.label = "File info",    .value = "info"       },
        {.label = "Add to menu",  .value = "add_to_menu"},
        {.label = "Delete",       .value = "delete"     },
        {.label = "Back",         .value = "back"       },
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
    /* Same as directory_actions, but for "/Network" specifically -- see
     * filemanager_network.h. Discovery already runs once up front (below),
     * so this is a manual way to re-run it without leaving and relaunching
     * filemanager, e.g. right after editing "/.ssh/config". */
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
     * empty/stale. See filemanager_network.h. */
    filemanager_network__refresh();

    bruce_dialog_render_params_t action_params = dialog__default_render_params(2);
    /* Lets a long press on a folder row return it below instead of
     * descending into it; out_long_press is left NULL since which entry
     * came back (probed via storage__list() further down) is all this loop
     * needs to know. */
    action_params.long_press_enabled = true;
    bool parent_entry = false;
    action_params.out_parent_entry = &parent_entry;
    /* Lets a configured "/config/filemanager.conf" "pathicons" entry (e.g.
     * "/Network") override the listing's default folder/per-extension icon
     * for that exact path -- see filemanager_pathicons.h. */
    action_params.icon_for_path = filemanager_pathicons__icon_for_path;
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
                filemanager_network__refresh();
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
        } else if (strcmp(action, "add_to_menu") == 0) {
            /* Same icon this row is actually showing in the browser above:
             * a configured pathicon override if one matches (see
             * filemanager_pathicons.h), else the per-extension icon every
             * other file listing falls back to -- launcher__add_menu_entry()
             * (core_sdk/launcher.h) owns picking where and reporting the
             * outcome, so this just hands it the file's name, icon, and
             * path as the label/icon/command. */
            char icon[BRUCE_DIALOG_ICON_NAME_MAX];
            bool has_icon = filemanager_pathicons__icon_for_path(path, false, icon, sizeof(icon), NULL);
            result = launcher__add_menu_entry(
                filemanager__basename(path), has_icon ? icon : app_runner__icon_for_path(path), path
            );
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
