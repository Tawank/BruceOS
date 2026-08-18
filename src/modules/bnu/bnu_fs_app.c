#include "bnu_app.h"
#include "bnu_internal.h"

#include "args.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/* Filesystem commands: pwd, ls, mkdir, touch, rm, cat. */

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
