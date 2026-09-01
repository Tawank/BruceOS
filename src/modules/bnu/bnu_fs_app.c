#include "bnu_app.h"
#include "bnu_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/filetype.h"
#include "core_sdk/format.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"

/* Filesystem commands: pwd, ls, mkdir, touch, rm, cp, mv, cat, file, head, tail, du. */

/* Broad, extension-driven color categories, in the spirit of GNU ls's
 * LS_COLORS: directories get their own color regardless of name; everything
 * else is bucketed from filetype__lookup_extension()'s mimetype/program
 * fields (cheap -- extension-table only, no I/O). NULL means "no color",
 * left to the terminal's default. */
static const char *bnu_ls__color_for(const bruce_storage_entry_t *entry) {
    if (entry->type == BRUCE_STORAGE_ENTRY_DIRECTORY) return "\033[1;34m";
    bruce_filetype_info_t info;
    if (filetype__lookup_extension(entry->name, &info) != BRUCE_OK) return NULL;
    if (strncmp(info.mimetype, "image/", 6) == 0 || strncmp(info.mimetype, "audio/", 6) == 0 ||
        strncmp(info.mimetype, "video/", 6) == 0) {
        return "\033[35m";
    }
    if (strcmp(info.mimetype, "application/zip") == 0 || strcmp(info.mimetype, "application/gzip") == 0 ||
        strcmp(info.mimetype, "application/x-tar") == 0) {
        return "\033[1;31m";
    }
    if (info.program[0] != '\0') return "\033[1;32m";
    return NULL;
}

static void bnu_ls__print_name(const bruce_storage_entry_t *entry, bool color) {
    const char *code = color ? bnu_ls__color_for(entry) : NULL;
    if (code != NULL) stdio__printf("%s", code);
    stdio__printf("%s%s", entry->name, entry->type == BRUCE_STORAGE_ENTRY_DIRECTORY ? "/" : "");
    if (code != NULL) stdio__printf("\033[0m");
}

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
    ap_add_flag(parser, "l");
    ap_set_opt_help(parser, "l", "Use long listing format");
    ap_add_flag(parser, "a");
    ap_set_opt_help(parser, "a", "Show hidden entries too");
    ap_add_flag(parser, "h");
    ap_set_opt_help(parser, "h", "Human-readable sizes (e.g. 4.0K, 1.2M)");
    ap_add_str_opt(parser, "color", "auto");
    ap_set_opt_help(parser, "color", "Colorize output: always, auto, or never (default: auto)");
    ap_add_optional_arg(parser, "path", "Path to list (defaults to the working directory)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool long_format = ap_found(parser, "l");
    bool show_hidden = ap_found(parser, "a");
    bool human_readable = ap_found(parser, "h");
    const char *color_mode = ap_get_str_value(parser, "color");
    bool color_always = strcmp(color_mode, "always") == 0;
    bool color_auto = strcmp(color_mode, "auto") == 0;
    if (!color_always && !color_auto && strcmp(color_mode, "never") != 0) {
        stdio__printf("ls: invalid --color value '%s' (expected always, auto, or never)\n", color_mode);
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
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
        /* Auto only colorizes an interactive terminal; always intentionally
         * preserves ANSI escapes through pipes and redirection. */
        bool color = color_always || (color_auto && tty__isatty());
        for (size_t i = 0; i < count; ++i) {
            if (!show_hidden && entries[i].name[0] == '.') continue;
            if (long_format) {
                char size_text[16] = "-";
                if (entries[i].type != BRUCE_STORAGE_ENTRY_DIRECTORY) {
                    bnu__format_size((uint32_t)entries[i].size, human_readable, size_text, sizeof(size_text));
                }
                stdio__printf(
                    "%c %8s  ", entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY ? 'd' : '-', size_text
                );
                bnu_ls__print_name(&entries[i], color);
                stdio__printf("\n");
            } else {
                bnu_ls__print_name(&entries[i], color);
                stdio__printf("\n");
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

/* Shared body of cp and mv: both take exactly a source and destination path
 * (no globbing, no directory destinations, no recursive directory copy -
 * matching rm/mkdir/touch's own minimal, single-path style above) and print
 * the same "cmd: from -> to: error N" shape on failure. */
static int bnu__cp_mv_app_main(int argc, char **argv, bool move) {
    const char *command = move ? "mv" : "cp";
    ArgParser *parser = bnu__new_parser(move ? "Move or rename a file." : "Copy a file.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "source", move ? "File to move" : "File to copy");
    ap_add_required_arg(parser, "dest", "Destination path (must not already exist)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    char from[BRUCE_STORAGE_PATH_MAX];
    char to[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "source"), from) &&
                     bnu__resolve_path(ap_get_arg(parser, "dest"), to);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    bruce_result_t result = move ? storage__rename(from, to) : storage__copy(from, to);
    /* storage__rename() only fails BRUCE_ERR_INVALID_ARGUMENT when `from`
     * and `to` sit on different mounted filesystems (e.g. internal flash and
     * SD) - rename(2) can't cross that boundary, so fall back to a copy
     * (which streams through the public read/write API instead of a raw
     * rename and so doesn't care about mount boundaries) plus a remove,
     * matching a real cross-device mv. */
    if (move && result == BRUCE_ERR_INVALID_ARGUMENT) {
        result = storage__copy(from, to);
        if (result == BRUCE_OK) result = storage__remove(from);
    }
    if (result != BRUCE_OK) {
        stdio__printf("%s: %s -> %s: error %d\n", command, from, to, result);
        return result;
    }
    return BRUCE_OK;
}

int bnu_cp_app_main(int argc, char **argv) { return bnu__cp_mv_app_main(argc, argv, false); }

int bnu_mv_app_main(int argc, char **argv) { return bnu__cp_mv_app_main(argc, argv, true); }

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

int bnu_file_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Identify a file's type from its extension, contents, or shebang.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "mime i");
    ap_set_opt_help(parser, "mime", "Print only the MIME type, like `file -i`");
    ap_add_required_arg(parser, "file", "Path to identify");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool mime_only = ap_found(parser, "mime");
    int path_count = ap_count_args(parser);
    for (int i = 0; i < path_count; ++i) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
            ap_free(parser);
            return BRUCE_ERR_INVALID_PATH;
        }
        bruce_filetype_info_t info;
        bruce_result_t result = filetype__identify(path, &info);
        if (result != BRUCE_OK) {
            stdio__printf("file: %s: error %d\n", path, result);
            ap_free(parser);
            return result;
        }
        const char *mimetype = info.mimetype[0] != '\0' ? info.mimetype : "application/octet-stream";
        if (mime_only) {
            stdio__printf("%s: %s\n", path, mimetype);
        } else if (info.mimetype[0] != '\0') {
            stdio__printf("%s: %s (%s)\n", path, info.description, mimetype);
        } else {
            stdio__printf("%s: %s\n", path, info.description);
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

/* Old-style "-N" line count (e.g. "head -4"), as a shorthand for "-n N".
 * args.c never treats "-<digits>" as an option (see ap_parse_level()'s
 * isdigit() check, there so a leading "-4" doesn't get mistaken for a
 * cluster of short options) -- it lands as an ordinary positional instead,
 * which is what lets bnu__head_tail_app_main() below tell it apart from the
 * file argument itself. */
static bool bnu__head_tail_legacy_count(const char *arg, unsigned long *out_lines) {
    if (arg == NULL || arg[0] != '-' || arg[1] == '\0') return false;
    for (const char *c = arg + 1; *c != '\0'; ++c) {
        if (!isdigit((unsigned char)*c)) return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(arg + 1, &end, 10);
    if (end == NULL || *end != '\0') return false;
    *out_lines = value;
    return true;
}

/* Shared body of head and tail: parse "-n lines" (or the legacy "-N"
 * shorthand -- see bnu__head_tail_legacy_count()) plus the usual file-or-
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
    ap_set_opt_help(parser, "n", "Number of lines to print (also settable as e.g. -4)");
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "file", "File to read (reads stdin if omitted)");
    ap_unknown_options_as_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    ap_allow_extra_args(parser); /* to let a legacy "-N" arg sit alongside "file" -- resolved below. */
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int lines_arg = ap_get_int_value(parser, "n");
    unsigned long lines = lines_arg > 0 ? (unsigned long)lines_arg : 0;

    /* "file" isn't just ap_get_arg(parser, "file") here: with extra args
     * allowed, a legacy "-N" could occupy parsed_args[0] ahead of the real
     * file (e.g. "head -4 notes.txt"), so both positionals need sorting out
     * by shape rather than by position. */
    const char *path_arg = NULL;
    bool legacy_count_seen = false;
    for (int i = 0; i < ap_count_args(parser); ++i) {
        const char *value = ap_get_arg_at_index(parser, i);
        unsigned long legacy_lines = 0;
        if (!legacy_count_seen && bnu__head_tail_legacy_count(value, &legacy_lines)) {
            if (!ap_found(parser, "n")) lines = legacy_lines;
            legacy_count_seen = true;
            continue;
        }
        if (path_arg != NULL) {
            stdio__printf("%s: unexpected argument '%s'\n", command, value);
            ap_free(parser);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        path_arg = value;
    }
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

/* Adds up every file's size under `path` - directories recurse (see
 * bnu__du_walk_directory() below), and their own entries never contribute a
 * size of their own, only the plain files they (transitively) contain -
 * matching what real du actually measures. */
static bruce_result_t bnu__du_walk_directory(const char *path, uint64_t *out_bytes) {
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    bruce_storage_entry_t *entries = NULL;
    if (count > 0) {
        entries = memory__malloc(count * sizeof(*entries));
        if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
        result = storage__list(path, entries, count, &count);
    }
    for (size_t i = 0; result == BRUCE_OK && i < count; ++i) {
        char child[BRUCE_STORAGE_PATH_MAX];
        const char *name = entries[i].name;
        int written = strcmp(path, "/") == 0 ? snprintf(child, sizeof(child), "/%s", name)
                                              : snprintf(child, sizeof(child), "%s/%s", path, name);
        if (written < 0 || (size_t)written >= sizeof(child)) {
            result = BRUCE_ERR_RESOURCE_LIMIT;
            break;
        }
        if (entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY) {
            result = bnu__du_walk_directory(child, out_bytes);
        } else {
            *out_bytes += entries[i].size;
        }
    }
    memory__free(entries);
    return result;
}

/* `path` itself might be a plain file rather than a directory (e.g. `du
 * one_file.bin`), which bnu__du_walk_directory() above never sees - it only
 * ever stats *children* of a directory it already knows is one, straight
 * out of storage__list()'s own bruce_storage_entry_t.size, without a
 * dedicated open/seek per file. This top-level case is the one spot that
 * needs its own open+seek-to-end, the same way bnu_file_app_main() sizes a
 * single path. */
static bruce_result_t bnu__du_sum(const char *path, uint64_t *out_bytes) {
    *out_bytes = 0;
    size_t probe_count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &probe_count);
    if (result == BRUCE_OK) return bnu__du_walk_directory(path, out_bytes);
    if (result != BRUCE_ERR_IO) return result; /* not-a-directory is the only expected failure here */

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    uint64_t position = 0;
    result = storage__seek(file, 0, SEEK_END, &position);
    bruce_result_t close_result = storage__close(file);
    if (result != BRUCE_OK) return result;
    *out_bytes = position;
    return close_result;
}

int bnu_du_app_main(int argc, char **argv) {
    ArgParser *parser =
        bnu__new_parser("Show total disk usage of a file or directory tree (always summarized, like du -s).");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "h");
    ap_set_opt_help(parser, "h", "Show sizes in human-readable units (e.g. 8.2K, 1.3M)");
    ap_add_optional_arg(parser, "path", "File or directory to measure (defaults to the working directory)");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool human = ap_found(parser, "h");
    int given = ap_count_args(parser);
    int path_count = given > 0 ? given : 1; /* no args -> one pass over the working directory */

    bruce_result_t result = BRUCE_OK;
    for (int i = 0; i < path_count; ++i) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
            result = BRUCE_ERR_INVALID_PATH;
            break;
        }
        uint64_t bytes = 0;
        result = bnu__du_sum(path, &bytes);
        if (result != BRUCE_OK) {
            stdio__printf("du: %s: error %d\n", path, result);
            break;
        }
        char size_text[32];
        if (human) format__bytes_human(bytes, size_text, sizeof(size_text));
        else snprintf(size_text, sizeof(size_text), "%llu", (unsigned long long)bytes);
        stdio__printf("%-8s %s\n", size_text, path);
    }

    ap_free(parser);
    return result;
}
