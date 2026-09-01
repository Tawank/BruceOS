#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/environment.h"
#include "core_sdk/format.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Helpers shared by the bnu_*_app.c command implementations. Each file
 * groups a family of built-in commands (filesystem, disk, system, network);
 * this file holds only the pieces common to all of them.
 */

#define BNU__PWD_NAME "PWD"

const char *bnu__get_working_directory(void) {
    const char *value = environment__get(BNU__PWD_NAME);
    return value != NULL && value[0] == '/' ? value : "/";
}

int bnu__parse_failure(ArgParser *parser) {
    ap_status_t status = ap_get_status(parser);
    ap_free(parser);
    if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
    return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
}

ArgParser *bnu__new_parser(const char *helptext) {
    ArgParser *parser = ap_new_parser();
    if (parser != NULL) ap_set_helptext(parser, helptext);
    return parser;
}

bool bnu__resolve_path(const char *path, char *out_path) {
    char combined[BRUCE_STORAGE_PATH_MAX * 2];
    const char *working_directory = bnu__get_working_directory();
    if (path == NULL || path[0] == '\0') path = working_directory;
    int written = path[0] == '/' ? snprintf(combined, sizeof(combined), "%s", path)
                                 : snprintf(
                                       combined,
                                       sizeof(combined),
                                       "%s%s%s",
                                       working_directory,
                                       strcmp(working_directory, "/") == 0 ? "" : "/",
                                       path
                                   );
    if (written < 0 || (size_t)written >= sizeof(combined)) return false;

    size_t out_length = 1;
    out_path[0] = '/';
    out_path[1] = '\0';
    const char *cursor = combined;
    while (*cursor != '\0') {
        while (*cursor == '/') cursor++;
        const char *component = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - component);
        if (length == 0 || (length == 1 && component[0] == '.')) continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            while (out_length > 1 && out_path[out_length - 1] != '/') out_length--;
            if (out_length > 1) out_length--;
            out_path[out_length] = '\0';
            continue;
        }
        size_t separator = out_length > 1 ? 1u : 0u;
        if (out_length + separator + length >= BRUCE_STORAGE_PATH_MAX) return false;
        if (separator != 0) out_path[out_length++] = '/';
        memcpy(out_path + out_length, component, length);
        out_length += length;
        out_path[out_length] = '\0';
    }
    return true;
}

/* ArgParser's own "-h" only means --help while a command hasn't claimed "h"
 * for itself (see ap_parse_level()'s "-h"/ap_find_option() check in
 * args.c) - ls and free do, and pass their own ap_found(parser, "h") in as
 * `human` here, matching their real Unix -h namesakes; df and du claim it
 * the same way but call format__bytes_human() directly instead of this
 * helper, since a filesystem's byte counts can exceed uint32_t. */
void bnu__format_size(uint32_t bytes, bool human, char *output, size_t capacity) {
    if (human) format__bytes_human(bytes, output, capacity);
    else snprintf(output, capacity, "%u", (unsigned)bytes);
}

/* Identical in shape to grep's and head/tail's own private loaders (see
 * bnu_grep_app.c, bnu_fs_app.c) -- kept here as one shared copy for the
 * text-processing commands (uniq, cut, sort, rev) that all need the same
 * whole-buffer-then-scan approach, rather than adding a fourth near-copy. */
bruce_result_t bnu__load_path(const char *path, size_t max_bytes, const void **out_data, size_t *out_length) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK && size > max_bytes) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);

    const void *data = NULL;
    if (result == BRUCE_OK && size > 0) {
        data = memory__external_malloc((size_t)size);
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    unsigned char chunk[256];
    while (result == BRUCE_OK && offset < (size_t)size) {
        size_t read_size = 0;
        result = storage__read(file, chunk, sizeof(chunk), &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) result = memory__external_memcpy(data, offset, chunk, read_size);
        offset += read_size;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        if (data != NULL) (void)memory__external_free(data);
        return result;
    }
    *out_data = data;
    *out_length = offset;
    return BRUCE_OK;
}

bruce_result_t bnu__load_stdin(size_t size, const void **out_data, size_t *out_length) {
    const void *data = NULL;
    bruce_result_t result = BRUCE_OK;
    if (size > 0) {
        data = memory__external_malloc(size);
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    unsigned char chunk[256];
    while (result == BRUCE_OK && offset < size) {
        size_t want = size - offset > sizeof(chunk) ? sizeof(chunk) : size - offset;
        size_t read_size = 0;
        result = stdio__read(chunk, want, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) result = memory__external_memcpy(data, offset, chunk, read_size);
        offset += read_size;
    }
    if (result != BRUCE_OK) {
        if (data != NULL) (void)memory__external_free(data);
        return result;
    }
    *out_data = data;
    *out_length = offset;
    return BRUCE_OK;
}
