#include "webui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

#define WEBUI_UPLOAD_MAX (8u * 1024u * 1024u)
#define WEBUI_DELETE_DEPTH_MAX 16u
#define WEBUI_DELETE_ENTRIES_MAX 256u

bruce_result_t webui__list_files(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *query = webui__read_query(request);
    if (query == NULL) return webui__reply_text(request, 400, "Invalid query");
    char fs[16], folder[BRUCE_STORAGE_PATH_MAX], path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(query, "fs", fs, sizeof(fs)) &&
                 webui__form_value(query, "folder", folder, sizeof(folder)) &&
                 webui__storage_path(fs, folder, NULL, path, sizeof(path));
    memory__free(query);
    if (!valid) return webui__reply_text(request, 400, "Invalid path");

    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    if (count > SIZE_MAX / sizeof(bruce_storage_entry_t))
        return webui__reply_text(request, 503, "Directory too large");
    size_t capacity = count;
    void *entries_data = NULL;
    bool entries_external = false;
    if (count > 0) {
        bruce_result_t alloc_result =
            webui__alloc_direct(&entries_data, &entries_external, count * sizeof(bruce_storage_entry_t));
        if (alloc_result != BRUCE_OK) return webui__reply_text(request, 503, "Out of memory");
    }
    bruce_storage_entry_t *entries = entries_data;
    result = storage__list(path, entries, count, &count);
    if (result != BRUCE_OK || count > capacity) {
        webui__free_direct(entries_data, entries_external);
        return webui__reply_error(request, result != BRUCE_OK ? result : BRUCE_ERR_BUSY);
    }
    result = http_server_request__set_type(request, "text/plain; charset=utf-8");
    if (result == BRUCE_OK) {
        char parent[BRUCE_STORAGE_PATH_MAX + 8u];
        int length = snprintf(parent, sizeof(parent), "pa:%s:0\n", folder);
        if (length > 0 && (size_t)length < sizeof(parent))
            result = http_server_request__send_chunk(request, parent, (size_t)length);
        else result = BRUCE_ERR_INTERNAL;
    }
    for (size_t i = 0; result == BRUCE_OK && i < count; i++) {
        char size[24];
        webui__human_size(entries[i].size, size, sizeof(size));
        char line[BRUCE_STORAGE_NAME_MAX + 32u];
        int length = snprintf(
            line,
            sizeof(line),
            "%s:%s:%s\n",
            entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY ? "Fo" : "Fi",
            entries[i].name,
            entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY ? "0" : size
        );
        if (length < 0 || (size_t)length >= sizeof(line)) result = BRUCE_ERR_INTERNAL;
        else result = http_server_request__send_chunk(request, line, (size_t)length);
    }
    webui__free_direct(entries_data, entries_external);
    if (result == BRUCE_OK) result = http_server_request__finalize(request);
    return result;
}

static bruce_result_t webui__stream_file(
    bruce_http_server_request_t *request, const char *path, const char *type, bool attachment
) {
    bruce_file_id_t file;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    if (attachment) {
        const char *base = strrchr(path, '/');
        base = base != NULL ? base + 1 : path;
        char disposition[BRUCE_STORAGE_NAME_MAX + 32u];
        int length = snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", base);
        if (length < 0 || (size_t)length >= sizeof(disposition)) result = BRUCE_ERR_INVALID_PATH;
        else result = http_server_request__set_header(request, "Content-Disposition", disposition);
    }
    if (result == BRUCE_OK) result = http_server_request__set_type(request, type);
    uint8_t *buffer = result == BRUCE_OK ? memory__malloc(WEBUI_IO_CHUNK) : NULL;
    if (result == BRUCE_OK && buffer == NULL) result = BRUCE_ERR_NO_MEMORY;
    bool response_started = false;
    while (result == BRUCE_OK) {
        size_t count = 0;
        result = storage__read(file, buffer, WEBUI_IO_CHUNK, &count);
        if (result != BRUCE_OK || count == 0) break;
        result = http_server_request__send_chunk(request, buffer, count);
        if (result == BRUCE_OK) response_started = true;
    }
    memory__free(buffer);
    bruce_result_t close_result = storage__close(file);
    if (result == BRUCE_OK && close_result != BRUCE_OK) result = close_result;
    if (result == BRUCE_OK) {
        result = response_started ? http_server_request__finalize(request)
                                  : http_server_request__send(request, NULL, 0);
    }
    if (result != BRUCE_OK && !response_started) return webui__reply_error(request, result);
    return result;
}

static bruce_result_t webui__remove_tree(const char *path, unsigned depth) {
    if (depth > WEBUI_DELETE_DEPTH_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result == BRUCE_ERR_INVALID_ARGUMENT || result == BRUCE_ERR_IO) return storage__remove(path);
    if (result != BRUCE_OK) return result;
    if (count > WEBUI_DELETE_ENTRIES_MAX) return BRUCE_ERR_RESOURCE_LIMIT;

    while (result == BRUCE_OK && count > 0) {
        bruce_storage_entry_t entry;
        result = storage__list(path, &entry, 1, &count);
        if (result != BRUCE_OK || count == 0) break;
        char child[BRUCE_STORAGE_PATH_MAX];
        int length = snprintf(child, sizeof(child), "%s/%s", path, entry.name);
        if (length < 0 || (size_t)length >= sizeof(child)) result = BRUCE_ERR_INVALID_PATH;
        else if (entry.type == BRUCE_STORAGE_ENTRY_DIRECTORY) result = webui__remove_tree(child, depth + 1u);
        else result = storage__remove(child);
    }
    return result == BRUCE_OK ? storage__remove(path) : result;
}

static const char *webui__image_type(const char *path) {
    const char *extension = strrchr(path, '.');
    if (extension == NULL) return "application/octet-stream";
    if (strcmp(extension, ".jpg") == 0 || strcmp(extension, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(extension, ".png") == 0) return "image/png";
    if (strcmp(extension, ".gif") == 0) return "image/gif";
    if (strcmp(extension, ".bmp") == 0) return "image/bmp";
    if (strcmp(extension, ".webp") == 0) return "image/webp";
    return "application/octet-stream";
}

bruce_result_t webui__file(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *query = webui__read_query(request);
    if (query == NULL) return webui__reply_text(request, 400, "Invalid query");
    char fs[16], name[BRUCE_STORAGE_PATH_MAX], action[16], path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(query, "fs", fs, sizeof(fs)) &&
                 webui__form_value(query, "name", name, sizeof(name)) &&
                 webui__form_value(query, "action", action, sizeof(action)) &&
                 webui__storage_path(fs, name, NULL, path, sizeof(path));
    memory__free(query);
    if (!valid) return webui__reply_text(request, 400, "Invalid path");
    if (strcmp(action, "download") == 0)
        return webui__stream_file(request, path, "application/octet-stream", true);
    if (strcmp(action, "image") == 0)
        return webui__stream_file(request, path, webui__image_type(path), false);
    if (strcmp(action, "edit") == 0)
        return webui__stream_file(request, path, "text/plain; charset=utf-8", false);
    bruce_result_t result;
    if (strcmp(action, "create") == 0) result = storage__mkdir(path);
    else if (strcmp(action, "createfile") == 0) {
        bruce_file_id_t file;
        result = storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        );
        if (result == BRUCE_OK) result = storage__close(file);
    } else if (strcmp(action, "delete") == 0) {
        result = webui__remove_tree(path, 0);
    } else return webui__reply_text(request, 400, "Invalid action");
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    return webui__reply_text(request, 200, "OK");
}

bruce_result_t webui__rename(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *body = webui__read_body(request, 2048u);
    if (body == NULL) return webui__reply_text(request, 413, "Form too large");
    char fs[16], old_name[BRUCE_STORAGE_PATH_MAX], new_name[BRUCE_STORAGE_NAME_MAX];
    char old_path[BRUCE_STORAGE_PATH_MAX], new_path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(body, "fs", fs, sizeof(fs)) &&
                 webui__form_value(body, "filePath", old_name, sizeof(old_name)) &&
                 webui__form_value(body, "fileName", new_name, sizeof(new_name));
    memory__free(body);
    if (!valid || strchr(new_name, '/') != NULL || strcmp(new_name, ".") == 0 ||
        strcmp(new_name, "..") == 0 || !webui__storage_path(fs, old_name, NULL, old_path, sizeof(old_path)))
        return webui__reply_text(request, 400, "Invalid path");
    char browser_parent[BRUCE_STORAGE_PATH_MAX];
    snprintf(browser_parent, sizeof(browser_parent), "%s", old_name);
    char *slash = strrchr(browser_parent, '/');
    if (slash == NULL) return webui__reply_text(request, 400, "Invalid path");
    if (slash == browser_parent) slash[1] = '\0';
    else *slash = '\0';
    if (!webui__storage_path(fs, browser_parent, new_name, new_path, sizeof(new_path)))
        return webui__reply_text(request, 400, "Invalid path");
    bruce_result_t result = storage__rename(old_path, new_path);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    return webui__reply_text(request, 200, "OK");
}

/* The editor form is "fs=...&name=...&content=<percent-encoded text>". The
 * request body is read straight into a memory__external_malloc() allocation
 * (PSRAM, swap, or internal RAM -- whichever memory__get_stats()-informed
 * webui__memory_cap() found room for) via chunked memory__external_memcpy()
 * calls, so a swap-backed allocation works here too: unlike a raw write, that
 * copy API doesn't need the destination to be directly addressable. fs/name
 * are short, so they're decoded straight off the returned pointer into small
 * stack buffers the ordinary way. "content" is decoded into a second,
 * separately-sized external allocation rather than in place, because swap has
 * no writable pointer to decode into -- the data must go through
 * memory__external_memcpy() again, this time via webui__decode_to_object(). */
bruce_result_t webui__edit(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;

    size_t content_length = http_server_request__content_length(request);
    if (content_length > webui__memory_cap())
        return webui__reply_text(request, 413, "Editor content exceeds available memory");

    const void *raw_data = memory__external_malloc(content_length + 1u);
    if (raw_data == NULL) return webui__reply_text(request, 503, "Out of memory for editor content");

    bruce_result_t result = webui__receive_into_object(request, raw_data, content_length);
    static const uint8_t terminator = 0;
    if (result == BRUCE_OK) result = memory__external_memcpy(raw_data, content_length, &terminator, 1u);
    if (result != BRUCE_OK) {
        (void)memory__external_free(raw_data);
        return webui__reply_error(request, result);
    }
    const char *body = raw_data;

    char fs[16], name[BRUCE_STORAGE_PATH_MAX], path[BRUCE_STORAGE_PATH_MAX];
    bool fs_valid = webui__form_value(body, "fs", fs, sizeof(fs));
    bool name_valid = webui__form_value(body, "name", name, sizeof(name));
    bool path_valid = fs_valid && name_valid && webui__storage_path(fs, name, NULL, path, sizeof(path));
    const char *content_start = NULL;
    size_t content_span = 0;
    bool content_found = webui__form_span(body, "content", &content_start, &content_span);
    if (!fs_valid || !name_valid || !path_valid || !content_found) {
        const char *reason = !fs_valid          ? "Invalid editor filesystem"
                             : !name_valid       ? "Invalid editor filename"
                             : !content_found    ? "Missing editor content"
                                                 : "Invalid editor path";
        printf(
            "webui__edit: warning: %s (fs=%d name=%d content=%d path=%d)\n",
            reason,
            fs_valid,
            name_valid,
            content_found,
            path_valid
        );
        (void)memory__external_free(raw_data);
        return webui__reply_text(request, 400, reason);
    }

    const void *content_data = NULL;
    size_t content_length_decoded = 0;
    if (content_span > 0) {
        content_data = memory__external_malloc(content_span);
        result = content_data != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
        if (result == BRUCE_OK)
            result = webui__decode_to_object(content_start, content_span, content_data, &content_length_decoded);
    }
    (void)memory__external_free(raw_data);
    if (result != BRUCE_OK) {
        if (content_data != NULL) (void)memory__external_free(content_data);
        return result == BRUCE_ERR_INVALID_ARGUMENT
                   ? webui__reply_text(request, 400, "Invalid or oversized editor content")
                   : webui__reply_error(request, result);
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result == BRUCE_OK && content_length_decoded > 0) {
        size_t offset = 0;
        while (result == BRUCE_OK && offset < content_length_decoded) {
            size_t written = 0;
            result = storage__write(
                file, (const uint8_t *)content_data + offset, content_length_decoded - offset, &written
            );
            if (result == BRUCE_OK && written == 0) result = BRUCE_ERR_IO;
            offset += written;
        }
    }
    if (content_data != NULL) (void)memory__external_free(content_data);
    if (result == BRUCE_OK) result = storage__close(file);
    else if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    return webui__reply_text(request, 200, "OK");
}

static bruce_result_t webui__mkdir_parents(char *path) {
    for (char *slash = path + 1; *slash != '\0'; slash++) {
        if (*slash != '/') continue;
        *slash = '\0';
        bruce_result_t result = storage__mkdir(path);
        *slash = '/';
        if (result != BRUCE_OK) return result;
    }
    return BRUCE_OK;
}

bruce_result_t webui__upload(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    size_t content_length = http_server_request__content_length(request);
    if (content_length > WEBUI_UPLOAD_MAX) return webui__reply_text(request, 413, "Upload exceeds 8 MiB");
    char *query = webui__read_query(request);
    if (query == NULL) return webui__reply_text(request, 400, "Invalid query");
    char fs[16], folder[BRUCE_STORAGE_PATH_MAX], name[BRUCE_STORAGE_PATH_MAX], path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(query, "fs", fs, sizeof(fs)) &&
                 webui__form_value(query, "folder", folder, sizeof(folder)) &&
                 webui__form_value(query, "name", name, sizeof(name)) && name[0] != '/' &&
                 webui__storage_path(fs, folder, name, path, sizeof(path));
    memory__free(query);
    if (!valid) return webui__reply_text(request, 400, "Invalid upload path");
    bruce_result_t result = webui__mkdir_parents(path);
    bruce_file_id_t file = 0;
    if (result == BRUCE_OK)
        result = storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        );
    uint8_t *buffer = result == BRUCE_OK ? memory__malloc(WEBUI_IO_CHUNK) : NULL;
    if (result == BRUCE_OK && buffer == NULL) result = BRUCE_ERR_NO_MEMORY;
    size_t received = 0;
    while (result == BRUCE_OK && received < content_length) {
        size_t wanted = content_length - received;
        if (wanted > WEBUI_IO_CHUNK) wanted = WEBUI_IO_CHUNK;
        int count = http_server_request__recv(request, buffer, wanted);
        if (count <= 0) {
            result = count < 0 ? (bruce_result_t)count : BRUCE_ERR_IO;
            break;
        }
        size_t offset = 0;
        while (result == BRUCE_OK && offset < (size_t)count) {
            size_t written = 0;
            result = storage__write(file, buffer + offset, (size_t)count - offset, &written);
            if (result == BRUCE_OK && written == 0) result = BRUCE_ERR_IO;
            offset += written;
        }
        received += (size_t)count;
    }
    memory__free(buffer);
    if (file != 0) {
        bruce_result_t close_result = storage__close(file);
        if (result == BRUCE_OK) result = close_result;
    }
    if (result != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__remove(path);
        return webui__reply_error(request, result);
    }
    return webui__reply_text(request, 200, "Upload complete");
}
