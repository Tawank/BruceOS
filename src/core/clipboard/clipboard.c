#include "core_sdk/clipboard.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

/* System-lifetime clipboard state, deliberately independent of any
 * process's own memory: the copying process may exit long before something
 * else pastes, so this uses plain malloc/free/strdup (freed here, by
 * whichever call replaces or clears the clipboard next) rather than
 * memory__malloc()'s process-owned tracked heap - see core/config/config.c
 * and core/process/process_environment.c for the same pattern applied to
 * other global, cross-process Core state. */
typedef struct {
    bruce_clipboard_kind_t kind;
    char *text;
    char **file_paths;
    size_t file_count;
    bruce_clipboard_file_mode_t file_mode;
} clipboard__state_t;

static StaticSemaphore_t s_clipboard_mutex_storage;
static SemaphoreHandle_t s_clipboard_mutex;
static portMUX_TYPE s_clipboard_init_mux = portMUX_INITIALIZER_UNLOCKED;
static clipboard__state_t s_state;

static void clipboard__ensure_mutex(void) {
    if (s_clipboard_mutex != NULL) return;
    portENTER_CRITICAL(&s_clipboard_init_mux);
    if (s_clipboard_mutex == NULL) { s_clipboard_mutex = xSemaphoreCreateMutexStatic(&s_clipboard_mutex_storage); }
    portEXIT_CRITICAL(&s_clipboard_init_mux);
}

static void clipboard__lock(void) {
    clipboard__ensure_mutex();
    xSemaphoreTake(s_clipboard_mutex, portMAX_DELAY);
}

static void clipboard__unlock(void) { xSemaphoreGive(s_clipboard_mutex); }

static void clipboard__clear_locked(void) {
    free(s_state.text);
    s_state.text = NULL;
    for (size_t i = 0; i < s_state.file_count; ++i) free(s_state.file_paths[i]);
    free(s_state.file_paths);
    s_state.file_paths = NULL;
    s_state.file_count = 0;
    s_state.file_mode = BRUCE_CLIPBOARD_FILE_COPY;
    s_state.kind = BRUCE_CLIPBOARD_EMPTY;
}

void clipboard__clear(void) {
    clipboard__lock();
    clipboard__clear_locked();
    clipboard__unlock();
}

bruce_clipboard_kind_t clipboard__kind(void) {
    clipboard__lock();
    bruce_clipboard_kind_t kind = s_state.kind;
    clipboard__unlock();
    return kind;
}

bruce_result_t clipboard__set_text(const char *text) {
    if (text == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    /* Duplicate before touching the existing clipboard, so a failed copy
     * leaves whatever was there before intact instead of losing it. */
    char *copy = strdup(text);
    if (copy == NULL) return BRUCE_ERR_NO_MEMORY;

    clipboard__lock();
    clipboard__clear_locked();
    s_state.kind = BRUCE_CLIPBOARD_TEXT;
    s_state.text = copy;
    clipboard__unlock();
    return BRUCE_OK;
}

const char *clipboard__get_text(void) {
    clipboard__lock();
    const char *text = s_state.kind == BRUCE_CLIPBOARD_TEXT ? s_state.text : NULL;
    clipboard__unlock();
    return text;
}

bruce_result_t clipboard__set_files(const char *const *paths, size_t count, bruce_clipboard_file_mode_t mode) {
    if (paths == NULL || count == 0 || count > BRUCE_CLIPBOARD_MAX_FILES) return BRUCE_ERR_INVALID_ARGUMENT;
    if (mode != BRUCE_CLIPBOARD_FILE_COPY && mode != BRUCE_CLIPBOARD_FILE_CUT) return BRUCE_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        if (paths[i] == NULL || paths[i][0] != '/' || strlen(paths[i]) >= BRUCE_STORAGE_PATH_MAX) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    }

    char **copies = malloc(count * sizeof(*copies));
    if (copies == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t duplicated = 0;
    bruce_result_t result = BRUCE_OK;
    for (; duplicated < count; ++duplicated) {
        copies[duplicated] = strdup(paths[duplicated]);
        if (copies[duplicated] == NULL) {
            result = BRUCE_ERR_NO_MEMORY;
            break;
        }
    }
    if (result != BRUCE_OK) {
        for (size_t i = 0; i < duplicated; ++i) free(copies[i]);
        free(copies);
        return result;
    }

    clipboard__lock();
    clipboard__clear_locked();
    s_state.kind = BRUCE_CLIPBOARD_FILES;
    s_state.file_paths = copies;
    s_state.file_count = count;
    s_state.file_mode = mode;
    clipboard__unlock();
    return BRUCE_OK;
}

size_t clipboard__file_count(void) {
    clipboard__lock();
    size_t count = s_state.kind == BRUCE_CLIPBOARD_FILES ? s_state.file_count : 0;
    clipboard__unlock();
    return count;
}

const char *clipboard__get_file(size_t index) {
    clipboard__lock();
    const char *path =
        s_state.kind == BRUCE_CLIPBOARD_FILES && index < s_state.file_count ? s_state.file_paths[index] : NULL;
    clipboard__unlock();
    return path;
}

bruce_clipboard_file_mode_t clipboard__file_mode(void) {
    clipboard__lock();
    bruce_clipboard_file_mode_t mode = s_state.file_mode;
    clipboard__unlock();
    return mode;
}

/* -------------------------------------------------------------------------- */
/* clipboard__paste_files() and its recursive copy/move engine                */
/* -------------------------------------------------------------------------- */

static bruce_result_t clipboard__join_path(const char *directory, const char *name, char *out) {
    size_t directory_length = strlen(directory);
    bool has_trailing_slash = directory_length > 0 && directory[directory_length - 1] == '/';
    int written =
        snprintf(out, BRUCE_STORAGE_PATH_MAX, has_trailing_slash ? "%s%s" : "%s/%s", directory, name);
    return written < 0 || (size_t)written >= BRUCE_STORAGE_PATH_MAX ? BRUCE_ERR_INVALID_PATH : BRUCE_OK;
}

/* Trailing-slash-free paths only (every path this file ever builds or is
 * handed already is), so the name is always everything after the last '/'. */
static const char *clipboard__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

/* storage__list() can only ever succeed on a directory (opendir() fails
 * with ENOTDIR -> BRUCE_ERR_IO on a plain file) - the same idiom
 * shell_builtins__cd() already relies on to validate a `cd` target. */
static bruce_result_t clipboard__is_directory(const char *path, bool *out_is_directory) {
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result == BRUCE_OK) {
        *out_is_directory = true;
        return BRUCE_OK;
    }
    if (result == BRUCE_ERR_IO) {
        *out_is_directory = false;
        return BRUCE_OK;
    }
    return result;
}

/* True when `path` is `ancestor` itself or nested under it - used to refuse
 * pasting a directory into its own subtree, which would otherwise recurse
 * into the very output it just created until storage fills up. */
static bool clipboard__is_within(const char *ancestor, const char *path) {
    size_t ancestor_length = strlen(ancestor);
    if (strncmp(ancestor, path, ancestor_length) != 0) return false;
    return path[ancestor_length] == '\0' || path[ancestor_length] == '/';
}

static bruce_result_t clipboard__copy_file_bytes(const char *src, const char *dst) {
    bruce_file_id_t in = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(src, BRUCE_STORAGE_OPEN_READ, &in);
    if (result != BRUCE_OK) return result;
    bruce_file_id_t out = BRUCE_FILE_ID_INVALID;
    result = storage__open(dst, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE, &out);
    if (result != BRUCE_OK) {
        storage__close(in);
        return result;
    }

    unsigned char buffer[512];
    while (result == BRUCE_OK) {
        size_t read_size = 0;
        result = storage__read(in, buffer, sizeof(buffer), &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        size_t total_written = 0;
        while (total_written < read_size) {
            size_t written = 0;
            result = storage__write(out, buffer + total_written, read_size - total_written, &written);
            if (result != BRUCE_OK) break;
            if (written == 0) {
                result = BRUCE_ERR_IO;
                break;
            }
            total_written += written;
        }
    }
    bruce_result_t close_in = storage__close(in);
    bruce_result_t close_out = storage__close(out);
    if (result != BRUCE_OK) return result;
    if (close_in != BRUCE_OK) return close_in;
    return close_out;
}

static bruce_result_t clipboard__copy_entry(const char *src, const char *dst, bool move) {
    bool destination_exists = false;
    bruce_result_t result = storage__exists(dst, &destination_exists);
    if (result != BRUCE_OK) return result;
    if (destination_exists) return BRUCE_ERR_ALREADY_EXISTS;

    bool is_directory = false;
    result = clipboard__is_directory(src, &is_directory);
    if (result != BRUCE_OK) return result;

    if (!is_directory) {
        result = clipboard__copy_file_bytes(src, dst);
    } else {
        result = storage__mkdir(dst);
        size_t count = 0;
        if (result == BRUCE_OK) result = storage__list(src, NULL, 0, &count);
        bruce_storage_entry_t *entries = NULL;
        if (result == BRUCE_OK && count > 0) {
            entries = malloc(count * sizeof(*entries));
            result = entries != NULL ? storage__list(src, entries, count, &count) : BRUCE_ERR_NO_MEMORY;
        }
        for (size_t i = 0; result == BRUCE_OK && i < count; ++i) {
            char child_src[BRUCE_STORAGE_PATH_MAX];
            char child_dst[BRUCE_STORAGE_PATH_MAX];
            result = clipboard__join_path(src, entries[i].name, child_src);
            if (result == BRUCE_OK) result = clipboard__join_path(dst, entries[i].name, child_dst);
            if (result == BRUCE_OK) result = clipboard__copy_entry(child_src, child_dst, move);
        }
        free(entries);
    }
    /* A move's source is only ever removed after everything under it (for a
     * directory, every child, recursively) has already been copied out, so
     * by the time this runs the directory case above has emptied `src`
     * enough for storage__remove() to accept it. */
    if (result == BRUCE_OK && move) result = storage__remove(src);
    return result;
}

bruce_result_t clipboard__paste_files(const char *target_directory) {
    if (target_directory == NULL || target_directory[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    bool target_exists = false;
    bruce_result_t result = storage__exists(target_directory, &target_exists);
    if (result != BRUCE_OK) return result;
    if (!target_exists) return BRUCE_ERR_NOT_FOUND;

    /* Snapshot the clipboard under the lock rather than holding it for the
     * whole (potentially slow) file copy below, which would block every
     * other clipboard__ call for as long as the copy takes. */
    clipboard__lock();
    if (s_state.kind != BRUCE_CLIPBOARD_FILES) {
        clipboard__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    size_t count = s_state.file_count;
    bool move = s_state.file_mode == BRUCE_CLIPBOARD_FILE_CUT;
    char **sources = malloc(count * sizeof(*sources));
    bruce_result_t snapshot_result = sources != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
    size_t duplicated = 0;
    for (; snapshot_result == BRUCE_OK && duplicated < count; ++duplicated) {
        sources[duplicated] = strdup(s_state.file_paths[duplicated]);
        if (sources[duplicated] == NULL) snapshot_result = BRUCE_ERR_NO_MEMORY;
    }
    clipboard__unlock();

    if (snapshot_result != BRUCE_OK) {
        for (size_t i = 0; i < duplicated; ++i) free(sources[i]);
        free(sources);
        return snapshot_result;
    }

    result = BRUCE_OK;
    for (size_t i = 0; i < count && result == BRUCE_OK; ++i) {
        if (clipboard__is_within(sources[i], target_directory)) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
            break;
        }
        char destination[BRUCE_STORAGE_PATH_MAX];
        result = clipboard__join_path(target_directory, clipboard__basename(sources[i]), destination);
        if (result == BRUCE_OK) result = clipboard__copy_entry(sources[i], destination, move);
    }
    for (size_t i = 0; i < count; ++i) free(sources[i]);
    free(sources);
    return result;
}
