#include "appdata.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config/config.h"
#include "core/permission/permission.h"
#include "core/process/process.h"
#include "core/storage/storage.h"
#include "core_sdk/permission.h"
#include "core_sdk/storage.h"

#define APPDATA__FOLDER "/appdata"
#define APPDATA__NAME_MAX 40

/* "maps.elf" -> "maps". */
static bool appdata__name(const char *app, char *out_name, size_t capacity) {
    const char *dot = strrchr(app, '.');
    size_t length = dot != NULL && dot != app ? (size_t)(dot - app) : strlen(app);
    size_t written = 0;
    for (size_t i = 0; i < length && written + 1 < capacity; ++i) {
        char c = app[i];
        out_name[written++] = isalnum((unsigned char)c) || c == '_' || c == '-' ? c : '_';
    }
    out_name[written] = '\0';
    return written > 0;
}

static bool appdata__default_path(const char *app, bool on_sd, char *out_path, size_t capacity) {
    char name[APPDATA__NAME_MAX];
    if (!appdata__name(app, name, sizeof(name))) return false;
    int written =
        snprintf(out_path, capacity, "%s" APPDATA__FOLDER "/%s", on_sd ? STORAGE__SD_MOUNT_PATH : "", name);
    return written > 0 && (size_t)written < capacity;
}

/* The saved folder, or the default one. `assign` saves the default so that a
 * later change of the default location does not move existing apps. */
static bruce_result_t
appdata__folder(const char *app, bool built_in, bool assign, char *out_path, size_t capacity) {
    if (app == NULL || app[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    if (!built_in && permission__get_appdata(app, out_path, capacity)) return BRUCE_OK;
    bool on_sd = !built_in && config__get_appdata_on_sd() && storage__sd_is_ready();
    if (!appdata__default_path(app, on_sd, out_path, capacity)) return BRUCE_ERR_INVALID_ARGUMENT;
    return assign && !built_in ? permission__set_appdata(app, out_path) : BRUCE_OK;
}

/* Creates `folder` and any missing parents. */
static bruce_result_t appdata__ensure(const char *folder) {
    if (storage__is_sd_path(folder) && !storage__sd_is_ready()) return BRUCE_ERR_INVALID_STATE;
    if (storage__exists_internal(folder)) return BRUCE_OK;
    char partial[PERMISSION__APPDATA_PATH_MAX];
    size_t length = strlen(folder);
    if (length >= sizeof(partial)) return BRUCE_ERR_INVALID_PATH;
    for (size_t i = 1; i <= length; ++i) {
        if (folder[i] != '/' && folder[i] != '\0') continue;
        memcpy(partial, folder, i);
        partial[i] = '\0';
        if (!storage__mkdir_internal(partial)) return BRUCE_ERR_IO;
    }
    return BRUCE_OK;
}

static bruce_result_t appdata__caller_folder(char *out_path, size_t capacity) {
    bool built_in = false;
    char app[BRUCE_PERMISSION_FILE_NAME_MAX] = {0};
    if (process_registry__current_context(&built_in, app, sizeof(app), NULL) != BRUCE_OK || app[0] == '\0') {
        return BRUCE_ERR_INVALID_PATH;
    }
    bruce_result_t result = appdata__folder(app, built_in, true, out_path, capacity);
    return result == BRUCE_OK ? appdata__ensure(out_path) : result;
}

bruce_result_t appdata__resolve(const char *path, char **out_path) {
    *out_path = NULL;
    if (path == NULL || path[0] == '/') return BRUCE_ERR_INVALID_PATH;
    char folder[PERMISSION__APPDATA_PATH_MAX];
    bruce_result_t result = appdata__caller_folder(folder, sizeof(folder));
    if (result != BRUCE_OK) return result;
    size_t size = strlen(folder) + 1 + strlen(path) + 1;
    if (size > BRUCE_STORAGE_PATH_MAX) return BRUCE_ERR_INVALID_PATH;
    *out_path = malloc(size);
    if (*out_path == NULL) return BRUCE_ERR_NO_MEMORY;
    snprintf(*out_path, size, path[0] == '\0' ? "%s" : "%s/%s", folder, path);
    return BRUCE_OK;
}

static bool appdata__is_relative(const char *name) {
    return name != NULL && name[0] != '\0' && name[0] != '/';
}

bruce_result_t appdata__get_path(char *out_path, size_t capacity) {
    if (out_path == NULL || capacity == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    char folder[PERMISSION__APPDATA_PATH_MAX];
    bruce_result_t result = appdata__caller_folder(folder, sizeof(folder));
    if (result != BRUCE_OK) return result;
    if (strlen(folder) >= capacity) return BRUCE_ERR_INVALID_ARGUMENT;
    strcpy(out_path, folder);
    return BRUCE_OK;
}

bruce_result_t appdata__read(const char *name, void *buffer, size_t capacity, size_t *out_size) {
    if (!appdata__is_relative(name) || (buffer == NULL && capacity != 0) || out_size == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(name, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    while (result == BRUCE_OK && *out_size < capacity) {
        size_t read = 0;
        result = storage__read(file, (char *)buffer + *out_size, capacity - *out_size, &read);
        if (read == 0) break;
        *out_size += read;
    }
    if (result == BRUCE_OK && *out_size == capacity) {
        char extra;
        size_t read = 0;
        if (storage__read(file, &extra, 1, &read) == BRUCE_OK && read > 0) result = BRUCE_ERR_RESOURCE_LIMIT;
    }
    storage__close(file);
    return result;
}

bruce_result_t appdata__write(const char *name, const void *data, size_t size) {
    if (!appdata__is_relative(name) || (data == NULL && size != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    char temporary[BRUCE_STORAGE_PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", name) >= (int)sizeof(temporary)) {
        return BRUCE_ERR_INVALID_PATH;
    }
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        temporary, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) return result;
    size_t total = 0;
    while (result == BRUCE_OK && total < size) {
        size_t written = 0;
        result = storage__write(file, (const char *)data + total, size - total, &written);
        if (result == BRUCE_OK && written == 0) result = BRUCE_ERR_IO;
        total += written;
    }
    bruce_result_t closed = storage__close(file);
    if (result == BRUCE_OK) result = closed;
    if (result == BRUCE_OK) {
        bruce_result_t removed = storage__remove(name);
        if (removed != BRUCE_OK && removed != BRUCE_ERR_NOT_FOUND) result = removed;
    }
    if (result == BRUCE_OK) result = storage__rename(temporary, name);
    if (result != BRUCE_OK) storage__remove(temporary);
    return result;
}

bruce_result_t appdata__remove(const char *name) {
    if (!appdata__is_relative(name)) return BRUCE_ERR_INVALID_ARGUMENT;
    return storage__remove(name);
}

bruce_result_t appdata__get_usage(size_t *out_bytes) {
    if (out_bytes == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    char folder[PERMISSION__APPDATA_PATH_MAX];
    bruce_result_t result = appdata__caller_folder(folder, sizeof(folder));
    return result == BRUCE_OK ? storage__tree_size_internal(folder, out_bytes) : result;
}

bruce_result_t appdata__app_path(const char *app, char *out_path, size_t capacity) {
    if (out_path == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    return appdata__folder(app, false, false, out_path, capacity);
}

bool appdata__app_on_sd(const char *app) {
    char folder[PERMISSION__APPDATA_PATH_MAX];
    return appdata__app_path(app, folder, sizeof(folder)) == BRUCE_OK && storage__is_sd_path(folder);
}

bruce_result_t appdata__app_usage(const char *app, size_t *out_bytes) {
    if (out_bytes == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_bytes = 0;
    char folder[PERMISSION__APPDATA_PATH_MAX];
    bruce_result_t result = appdata__app_path(app, folder, sizeof(folder));
    if (result != BRUCE_OK) return result;
    if (storage__is_sd_path(folder) && !storage__sd_is_ready()) return BRUCE_ERR_INVALID_STATE;
    return storage__tree_size_internal(folder, out_bytes);
}

bruce_result_t appdata__move(const char *app, bool to_sd) {
    char from[PERMISSION__APPDATA_PATH_MAX];
    char to[PERMISSION__APPDATA_PATH_MAX];
    bruce_result_t result = appdata__app_path(app, from, sizeof(from));
    if (result != BRUCE_OK) return result;
    if (!appdata__default_path(app, to_sd, to, sizeof(to))) return BRUCE_ERR_INVALID_ARGUMENT;
    if (strcmp(from, to) == 0) return permission__set_appdata(app, to);
    if (process_registry__is_app_running(app)) return BRUCE_ERR_BUSY;
    if ((storage__is_sd_path(from) || to_sd) && !storage__sd_is_ready()) return BRUCE_ERR_INVALID_STATE;

    bool had_data = storage__exists_internal(from);
    if (had_data) {
        result = storage__tree_remove_internal(to);
        if (result == BRUCE_OK) result = appdata__ensure(to);
        if (result == BRUCE_OK) result = storage__tree_copy_internal(from, to);
        if (result != BRUCE_OK) {
            storage__tree_remove_internal(to);
            return result;
        }
    }
    result = permission__set_appdata(app, to);
    if (result == BRUCE_OK && had_data) storage__tree_remove_internal(from);
    return result;
}

bruce_result_t appdata__clear(const char *app) {
    char folder[PERMISSION__APPDATA_PATH_MAX];
    bruce_result_t result = appdata__app_path(app, folder, sizeof(folder));
    if (result != BRUCE_OK) return result;
    if (process_registry__is_app_running(app)) return BRUCE_ERR_BUSY;
    if (storage__is_sd_path(folder) && !storage__sd_is_ready()) return BRUCE_ERR_INVALID_STATE;
    return storage__tree_remove_internal(folder);
}

bool appdata__default_on_sd(void) { return config__get_appdata_on_sd(); }

bruce_result_t appdata__set_default_on_sd(bool on_sd) { return config__set_appdata_on_sd(on_sd); }
