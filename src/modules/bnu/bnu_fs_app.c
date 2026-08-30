#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include "args.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/* Filesystem commands: pwd, ls, mkdir, touch, rm, cat, head, tail. */

int bnu_pwd_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Print the current working directory.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);
    stdio__printf("%s\n", bnu__get_working_directory());
    return BRUCE_OK;
}

int bnu_ls_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("List files and directories.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_optional_arg(parser, "path", "Path to list (defaults to the working directory)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "path"), path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) {
        stdio__printf("ls: %s: error %d\n", path, result);
        return result;
    }
    if (count == 0) return BRUCE_OK;
    bruce_storage_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = storage__list(path, entries, count, &count);
    if (result == BRUCE_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY) {
                stdio__printf("%-10s %s/\n", "<dir>", entries[i].name);
            } else {
                stdio__printf("%10u %s\n", (unsigned)entries[i].size, entries[i].name);
            }
        }
    }
    memory__free(entries);
    return result;
}

int bnu_mkdir_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Create a directory.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "directory", "Directory path to create");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "directory"), path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;
    bruce_result_t result = storage__mkdir(path);
    if (result != BRUCE_OK) {
        stdio__printf("mkdir: %s: error %d\n", path, result);
        return result;
    }
    return BRUCE_OK;
}

int bnu_touch_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Create a file if it does not exist.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "file", "File path to create");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "file"), path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;
    bruce_file_id_t file;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE, &file);
    if (result != BRUCE_OK) {
        stdio__printf("touch: %s: error %d\n", path, result);
        return result;
    }
    storage__close(file);
    return BRUCE_OK;
}

int bnu_rm_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Remove a file or empty directory.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "path", "Path to remove");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int path_count = ap_count_args(parser);
    for (int i = 0; i < path_count; ++i) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
            ap_free(parser);
            return BRUCE_ERR_INVALID_PATH;
        }
        bruce_result_t result = storage__remove(path);
        if (result != BRUCE_OK) {
            stdio__printf("rm: %s: error %d\n", path, result);
            ap_free(parser);
            return result;
        }
    }

    ap_free(parser);
    return BRUCE_OK;
}

static bruce_result_t bnu__cat_file(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    unsigned char buffer[256];
    while (result == BRUCE_OK) {
        size_t read_size = 0;
        result = storage__read(file, buffer, sizeof(buffer), &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        result = stdio__write(buffer, read_size);
    }

    bruce_result_t close_result = storage__close(file);
    return result != BRUCE_OK ? result : close_result;
}

int bnu_cat_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Print file contents.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "file", "File path to print");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int path_count = ap_count_args(parser);
    for (int i = 0; i < path_count; ++i) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
            ap_free(parser);
            return BRUCE_ERR_INVALID_PATH;
        }
        bruce_result_t result = bnu__cat_file(path);
        if (result != BRUCE_OK) {
            stdio__printf("cat: %s: error %d\n", path, result);
            ap_free(parser);
            return result;
        }
    }

    ap_free(parser);
    return BRUCE_OK;
}

/* head/tail load the whole file (or stdin) into one memory__external_malloc()
 * allocation -- same approach as bnu_less_app_main() (bnu_pager_app.c's
 * LESS_MAX_BYTES) -- then find the cut point by counting lines, rather than
 * streaming. Large enough for any text file worth viewing a piece of, backed
 * by PSRAM/swap rather than the internal heap. */
#define BNU_HEAD_TAIL_MAX_BYTES (512u * 1024u)
#define BNU_HEAD_TAIL_DEFAULT_LINES 10
#define BNU_HEAD_TAIL_CHUNK_SIZE 256

static bruce_result_t bnu__head_tail_load_path(const char *path, const void **out_data, size_t *out_length) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK && size > BNU_HEAD_TAIL_MAX_BYTES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);

    const void *data = NULL;
    if (result == BRUCE_OK && size > 0) {
        data = memory__external_malloc((size_t)size);
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    unsigned char chunk[BNU_HEAD_TAIL_CHUNK_SIZE];
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

static bruce_result_t bnu__head_tail_load_stdin(size_t size, const void **out_data, size_t *out_length) {
    const void *data = NULL;
    bruce_result_t result = BRUCE_OK;
    if (size > 0) {
        data = memory__external_malloc(size);
        if (data == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    size_t offset = 0;
    unsigned char chunk[BNU_HEAD_TAIL_CHUNK_SIZE];
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

/* Offset of the start of the `lines`-th line (0-based) counting from the
 * front, or `length` if the data has fewer than that many lines -- i.e. the
 * end of what "head -n lines" prints, and (via bnu__tail_offset() below) the
 * number of leading lines "tail -n lines" needs to skip. */
static size_t bnu__head_offset(const char *data, size_t length, unsigned long lines) {
    size_t offset = 0;
    for (unsigned long i = 0; i < lines && offset < length; ++i) {
        while (offset < length && data[offset] != '\n') offset++;
        if (offset < length) offset++;
    }
    return offset;
}

/* Total line count, counting a non-empty final line without a trailing
 * newline as one more line -- matches how real head/tail count lines. */
static unsigned long bnu__count_lines(const char *data, size_t length) {
    unsigned long count = 0;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\n') count++;
    }
    if (length > 0 && data[length - 1] != '\n') count++;
    return count;
}

/* Offset of the start of the last `lines` lines: skip (total - lines) lines
 * from the front via bnu__head_offset(), or none if the data has no more
 * than `lines` lines. */
static size_t bnu__tail_offset(const char *data, size_t length, unsigned long lines) {
    unsigned long total = bnu__count_lines(data, length);
    unsigned long skip = total > lines ? total - lines : 0;
    return bnu__head_offset(data, length, skip);
}

/* Shared body of head and tail: parse "-n lines" plus the usual file-or-
 * stdin argument (see bnu_less_app_main()'s "--stdin-size" handling, which
 * this mirrors), load the whole input, and print the requested slice. */
static int bnu__head_tail_app_main(int argc, char **argv, bool tail) {
    const char *command = tail ? "tail" : "head";
    ArgParser *parser = bnu__new_parser(
        tail ? "Print the last part of a file, or piped stdin."
             : "Print the first part of a file, or piped stdin."
    );
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_int_opt(parser, "n", BNU_HEAD_TAIL_DEFAULT_LINES);
    ap_set_opt_help(parser, "n", "Number of lines to print");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to read (reads stdin if omitted)");
    ap_unknown_options_as_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int lines_arg = ap_get_int_value(parser, "n");
    unsigned long lines = lines_arg > 0 ? (unsigned long)lines_arg : 0;
    const char *path_arg = ap_get_arg(parser, "file");
    const char *stdin_size_arg =
        ap_found(parser, "stdin-size") ? ap_get_str_value(parser, "stdin-size") : NULL;
    char *end = NULL;
    unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
    bool stdin_requested = stdin_size_arg != NULL;
    bool from_stdin = stdin_requested && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0' &&
                      parsed_stdin_size <= BNU_HEAD_TAIL_MAX_BYTES;
    char path[BRUCE_STORAGE_PATH_MAX] = {0};
    bool path_resolved = !from_stdin && path_arg != NULL && bnu__resolve_path(path_arg, path);
    ap_free(parser);

    if (stdin_requested && !from_stdin) {
        stdio__printf("%s: invalid --stdin-size\n", command);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!from_stdin && path_arg == NULL) {
        stdio__printf("%s: missing file operand\n", command);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!from_stdin && !path_resolved) return BRUCE_ERR_INVALID_PATH;

    const void *data = NULL;
    size_t length = 0;
    bruce_result_t result = from_stdin
                                 ? bnu__head_tail_load_stdin((size_t)parsed_stdin_size, &data, &length)
                                 : bnu__head_tail_load_path(path, &data, &length);
    if (result != BRUCE_OK) {
        stdio__printf("%s: %s: %s\n", command, from_stdin ? "-" : path, result__to_string(result));
        return result;
    }

    size_t start = 0, stop = length;
    if (data != NULL) {
        if (tail) start = bnu__tail_offset(data, length, lines);
        else stop = bnu__head_offset(data, length, lines);
    }
    if (stop > start) (void)stdio__write((const char *)data + start, stop - start);

    if (data != NULL) (void)memory__external_free(data);
    return BRUCE_OK;
}

int bnu_head_app_main(int argc, char **argv) { return bnu__head_tail_app_main(argc, argv, false); }

int bnu_tail_app_main(int argc, char **argv) { return bnu__head_tail_app_main(argc, argv, true); }
