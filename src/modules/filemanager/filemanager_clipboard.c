#include "filemanager_clipboard.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/clipboard.h"
#include "core_sdk/dialog.h"
#include "core_sdk/storage.h"

#include "filemanager_internal.h"

bruce_result_t filemanager__copy_entry(const char *path) {
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

bruce_result_t filemanager__paste_here(const char *directory) {
    bruce_clipboard_kind_t kind = clipboard__kind();
    if (kind == BRUCE_CLIPBOARD_FILES) return filemanager__paste_files_here(directory);
    if (kind == BRUCE_CLIPBOARD_BINARY) return filemanager__paste_binary_here(directory);
    return BRUCE_ERR_INVALID_STATE;
}
