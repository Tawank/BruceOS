#include "js_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

static bruce_result_t js_source__load_impl(
    const char *path, size_t max_size, bool transferable, js_source_t *out_source
) {
    if (path == NULL || max_size == 0 || out_source == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(out_source, 0, sizeof(*out_source));

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    uint64_t file_size = 0;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_END, &file_size);
    if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
    if (result != BRUCE_OK) return result;
    if (file_size == 0 || file_size > max_size) return BRUCE_ERR_RESOURCE_LIMIT;

    out_source->external_result = ext_mem_loader__stage_path(path, &out_source->external);
    if (out_source->external_result == BRUCE_OK) {
        if (out_source->external.size > max_size) {
            js_source__release(out_source);
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
        out_source->data = out_source->external.data;
        out_source->size = out_source->external.size;
        return BRUCE_OK;
    }

    out_source->internal =
        transferable ? malloc((size_t)file_size + 1u) : memory__malloc((size_t)file_size + 1u);
    if (out_source->internal == NULL) return BRUCE_ERR_NO_MEMORY;
    out_source->internal_tracked = !transferable;

    result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    size_t total = 0;
    while (result == BRUCE_OK && total < (size_t)file_size) {
        size_t received = 0;
        result = storage__read(file, out_source->internal + total, (size_t)file_size - total, &received);
        if (result == BRUCE_OK && received == 0) result = BRUCE_ERR_IO;
        total += received;
    }
    if (file != BRUCE_FILE_ID_INVALID) {
        bruce_result_t close_result = storage__close(file);
        if (result == BRUCE_OK) result = close_result;
    }
    if (result != BRUCE_OK) {
        bruce_result_t external_result = out_source->external_result;
        js_source__release(out_source);
        out_source->external_result = external_result;
        return result;
    }

    out_source->internal[total] = '\0';
    out_source->data = out_source->internal;
    out_source->size = total;
    return BRUCE_OK;
}

bruce_result_t js_source__load(const char *path, size_t max_size, js_source_t *out_source) {
    return js_source__load_impl(path, max_size, false, out_source);
}

bruce_result_t js_source__load_transferable(
    const char *path, size_t max_size, js_source_t *out_source
) {
    return js_source__load_impl(path, max_size, true, out_source);
}

bruce_result_t js_source__adopt(js_source_t *source) {
    if (source == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (source->external.memory.handle == 0) return BRUCE_OK;
    return ext_mem_loader__adopt_image(&source->external);
}

void js_source__release(js_source_t *source) {
    if (source == NULL) return;
    if (source->external.memory.handle != 0) (void)ext_mem_loader__release_image(&source->external);
    if (source->internal_tracked) {
        memory__free(source->internal);
    } else {
        free(source->internal);
    }
    memset(source, 0, sizeof(*source));
}
