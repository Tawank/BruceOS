#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/clipboard.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Shared clipboard commands: wl-copy, wl-paste (named after the Wayland
 * clipboard CLI tools they mirror, minus MIME type negotiation -- there's
 * only one clipboard slot here, not a set of typed offers). Content read by
 * wl-copy is classified the same way every other clipboard writer in the OS
 * decides: text unless it contains a NUL byte, in which case it becomes
 * BRUCE_CLIPBOARD_BINARY instead (see core_sdk/clipboard.h).
 */

/* Same bound the clipboard itself enforces (BRUCE_CLIPBOARD_MAX_BINARY_BYTES)
 * -- checked before loading input so an oversized pipe/file fails with a
 * clear message instead of a large up-front allocation that
 * clipboard__set_binary() would reject anyway. */
#define BNU_WL_COPY_MAX_BYTES BRUCE_CLIPBOARD_MAX_BINARY_BYTES

/* Shared with the hash/base64 commands' own copy of this pattern (see
 * bnu__hash_parse_stdin_size() in bnu_hash_app.c) -- kept as a separate copy
 * rather than exposed from there since each command needs its own name in
 * the printed error. */
static bool bnu__wl_copy_parse_stdin_size(const char *size_arg, unsigned long *out_size) {
    char *end = NULL;
    unsigned long parsed = strtoul(size_arg, &end, 10);
    if (size_arg[0] == '\0' || end == NULL || *end != '\0') {
        stdio__printf("wl-copy: invalid --stdin-size\n");
        return false;
    }
    *out_size = parsed;
    return true;
}

/* Loads the whole payload up front (like bnu_base64_app_main() does, and for
 * the same reason: classifying it as text vs. binary needs the complete
 * input first). Allocates one byte more than `size` so a text result can be
 * NUL-terminated in place without a second allocation. */
static bruce_result_t bnu__wl_copy_load_stdin(size_t size, uint8_t **out_data, size_t *out_length) {
    uint8_t *data = memory__malloc(size + 1);
    if (data == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t offset = 0;
    bruce_result_t result = BRUCE_OK;
    while (result == BRUCE_OK && offset < size) {
        size_t read_size = 0;
        result = stdio__read(data + offset, size - offset, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        offset += read_size;
    }
    if (result != BRUCE_OK) {
        memory__free(data);
        return result;
    }
    data[size] = '\0';
    *out_data = data;
    *out_length = size;
    return BRUCE_OK;
}

static bruce_result_t bnu__wl_copy_load_path(const char *path, uint8_t **out_data, size_t *out_length) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK && size > BNU_WL_COPY_MAX_BYTES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);

    uint8_t *data = NULL;
    if (result == BRUCE_OK) {
        data = memory__malloc((size_t)size + 1); /* +1, see bnu__wl_copy_load_stdin() */
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    while (result == BRUCE_OK && offset < (size_t)size) {
        size_t read_size = 0;
        result = storage__read(file, data + offset, (size_t)size - offset, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        offset += read_size;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        memory__free(data);
        return result;
    }
    data[offset] = '\0';
    *out_data = data;
    *out_length = offset;
    return BRUCE_OK;
}

/* Not a full UTF-8 validation -- like the rest of this OS's clipboard
 * writers, a byte value alone doesn't disqualify text, only an embedded NUL
 * does (the one thing every C string relies on not being there). */
static bool bnu__wl_copy_is_text(const uint8_t *data, size_t length) { return memchr(data, '\0', length) == NULL; }

static bruce_result_t bnu__wl_copy_store(const uint8_t *data, size_t length, const char *name) {
    if (bnu__wl_copy_is_text(data, length)) return clipboard__set_text((const char *)data);
    return clipboard__set_binary(data, length, name);
}

int bnu_wl_copy_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Copy stdin, or a file, to the shared clipboard.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "n", NULL);
    ap_set_opt_help(parser, "n", "Suggested filename, if the content ends up copied as binary data");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to copy (reads stdin if omitted)");
    ap_unknown_options_as_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *name_arg = ap_found(parser, "n") ? ap_get_str_value(parser, "n") : NULL;
    const char *path_arg = ap_get_arg(parser, "file");
    const char *stdin_size_arg = ap_found(parser, "stdin-size") ? ap_get_str_value(parser, "stdin-size") : NULL;
    bool from_stdin = path_arg == NULL;
    char path[BRUCE_STORAGE_PATH_MAX] = {0};
    bool path_resolved = from_stdin || bnu__resolve_path(path_arg, path);

    if (from_stdin && stdin_size_arg == NULL) {
        stdio__printf("wl-copy: missing file operand\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!from_stdin && !path_resolved) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_PATH;
    }
    unsigned long stdin_size = 0;
    if (from_stdin && !bnu__wl_copy_parse_stdin_size(stdin_size_arg, &stdin_size)) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (from_stdin && stdin_size > BNU_WL_COPY_MAX_BYTES) {
        stdio__printf("wl-copy: input too large (max %u bytes)\n", (unsigned)BNU_WL_COPY_MAX_BYTES);
        ap_free(parser);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    /* Falls back to the source file's own name when copying a real file and
     * no explicit -n override was given; piped stdin has no name of its own
     * to fall back to. */
    const char *name = name_arg;
    if (name == NULL && !from_stdin) {
        const char *slash = strrchr(path, '/');
        name = slash != NULL ? slash + 1 : path;
    }
    ap_free(parser);

    uint8_t *data = NULL;
    size_t length = 0;
    bruce_result_t result = from_stdin ? bnu__wl_copy_load_stdin((size_t)stdin_size, &data, &length)
                                        : bnu__wl_copy_load_path(path, &data, &length);
    if (result != BRUCE_OK) {
        stdio__printf("wl-copy: %s: %s\n", from_stdin ? "-" : path, result__to_string(result));
        return result;
    }

    result = bnu__wl_copy_store(data, length, name);
    memory__free(data);
    if (result != BRUCE_OK) stdio__printf("wl-copy: %s\n", result__to_string(result));
    return result;
}

int bnu_wl_paste_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Print the shared clipboard's contents to stdout.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "n");
    ap_set_opt_help(parser, "n", "Don't append a trailing newline to text content");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool no_newline = ap_found(parser, "n");
    ap_free(parser);

    bruce_clipboard_kind_t kind = clipboard__kind();
    if (kind == BRUCE_CLIPBOARD_TEXT) {
        const char *text = clipboard__get_text();
        bruce_result_t result = stdio__write(text, strlen(text));
        if (result == BRUCE_OK && !no_newline) result = stdio__write("\n", 1);
        if (result != BRUCE_OK) stdio__printf("wl-paste: %s\n", result__to_string(result));
        return result;
    }
    if (kind == BRUCE_CLIPBOARD_FILES) {
        /* No text/uri-list MIME type to speak of here -- just the plain
         * paths, one per line, same as the file manager's own clipboard
         * paste would write them out under. */
        size_t count = clipboard__file_count();
        for (size_t i = 0; i < count; ++i) {
            const char *path = clipboard__get_file(i);
            stdio__printf("%s\n", path != NULL ? path : "");
        }
        return BRUCE_OK;
    }
    if (kind == BRUCE_CLIPBOARD_BINARY) {
        const void *data = clipboard__get_binary();
        size_t size = clipboard__binary_size();
        bruce_result_t result = size > 0 ? stdio__write(data, size) : BRUCE_OK;
        if (result != BRUCE_OK) stdio__printf("wl-paste: %s\n", result__to_string(result));
        return result;
    }

    stdio__printf("wl-paste: clipboard is empty\n");
    return BRUCE_ERR_NOT_FOUND;
}
