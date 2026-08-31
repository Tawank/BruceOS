#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/archive.h"
#include "core_sdk/compress.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Archive commands, all thin wrappers over core_sdk/archive.h:
 *
 *   - tar: create/list/extract a ".tar.gz" (the only tar flavor
 *     core_sdk/archive.h supports - always gzip-compressed).
 *   - zip: create a ".zip".
 *   - unzip: list/extract a ".zip".
 *   - archive/archive-extract: format-agnostic list/extract, picking
 *     archive__zip_*() or archive__tar_gz_*() by `path`'s extension (see
 *     bnu__archive_is_zip()) - these are what extensions.conf's "program"/
 *     "actions" point a ".zip"/".tar.gz"/".tgz" file's "Open"/"Extract
 *     here" at, so opening one from the file manager doesn't need to know
 *     which format it is.
 */

/* True if `path` ends in ".zip" (case-insensitive, matching how
 * filetype__string_array_has_ci() compares extensions.conf's own
 * "extensions" lists); anything else (".tar.gz", ".tgz", or an
 * unrecognized suffix) is treated as tar.gz. */
static bool bnu__archive_is_zip(const char *path) {
    size_t length = strlen(path);
    return length >= 4 && strcasecmp(path + length - 4, ".zip") == 0;
}

static bool bnu__archive_print_entry(void *context, const bruce_archive_entry_t *entry) {
    (void)context;
    if (entry->is_directory) stdio__printf("%10s  %s\n", "<dir>", entry->name);
    else stdio__printf("%10zu  %s\n", entry->size, entry->name);
    return true;
}

/* `dest_dir` for an extract with no explicit destination: the archive's own
 * containing directory - i.e. "extract here" - which is guaranteed to
 * already exist (archive__*_extract() requires that), unlike a made-up
 * subdirectory name would be. */
static void bnu__archive_dirname(const char *path, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == NULL || slash == out) snprintf(out, out_size, "/");
    else *slash = '\0';
}

static bruce_result_t
bnu__archive_resolve_dest(const char *archive_path, const char *dest_arg, char *out_path, size_t out_size) {
    if (dest_arg != NULL) return bnu__resolve_path(dest_arg, out_path) ? BRUCE_OK : BRUCE_ERR_INVALID_PATH;
    bnu__archive_dirname(archive_path, out_path, out_size);
    return BRUCE_OK;
}

/* Resolves ap_count_args(parser)'s [start, count) positional args into a
 * heap-allocated array of BRUCE_STORAGE_PATH_MAX-byte paths, plus a
 * parallel `const char *` array pointing into it - the shape the
 * archive__zip_create()/archive__tar_gz_create() entry_paths parameter
 * wants. Both out params are memory__free()'d by the caller
 * (bnu__archive_free_entries() below); left NULL on failure, with nothing
 * to free. */
static bruce_result_t bnu__archive_resolve_entries(
    ArgParser *parser, int start, int count, char **out_paths, const char ***out_pointers
) {
    *out_paths = NULL;
    *out_pointers = NULL;
    int entry_count = count - start;
    if (entry_count <= 0) return BRUCE_ERR_INVALID_ARGUMENT;

    char *paths = memory__malloc((size_t)entry_count * BRUCE_STORAGE_PATH_MAX);
    const char **pointers = memory__malloc((size_t)entry_count * sizeof(char *));
    if (paths == NULL || pointers == NULL) {
        memory__free(paths);
        memory__free(pointers);
        return BRUCE_ERR_NO_MEMORY;
    }
    for (int i = 0; i < entry_count; ++i) {
        char *slot = paths + (size_t)i * BRUCE_STORAGE_PATH_MAX;
        if (!bnu__resolve_path(ap_get_arg_at_index(parser, start + i), slot)) {
            memory__free(paths);
            memory__free(pointers);
            return BRUCE_ERR_INVALID_PATH;
        }
        pointers[i] = slot;
    }
    *out_paths = paths;
    *out_pointers = pointers;
    return BRUCE_OK;
}

static void bnu__archive_free_entries(char *paths, const char **pointers) {
    memory__free(paths);
    memory__free(pointers);
}

/* ------------------------------------------------------------------------ */
/* tar                                                                      */
/* ------------------------------------------------------------------------ */

int bnu_tar_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Create, list, or extract a \".tar.gz\" archive.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "c");
    ap_set_opt_help(parser, "c", "Create a new archive");
    ap_add_flag(parser, "x");
    ap_set_opt_help(parser, "x", "Extract an existing archive");
    ap_add_flag(parser, "t");
    ap_set_opt_help(parser, "t", "List an archive's contents");
    ap_add_str_opt(parser, "f", NULL);
    ap_set_opt_help(parser, "f", "Archive path");
    ap_add_str_opt(parser, "C", NULL);
    ap_set_opt_help(parser, "C", "Directory to extract into (defaults to the archive's own directory)");
    ap_add_optional_arg(parser, "entry", "File(s)/directory(ies) to add when creating (-c)");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool create = ap_found(parser, "c");
    bool extract = ap_found(parser, "x");
    bool list = ap_found(parser, "t");
    const char *archive_arg = ap_found(parser, "f") ? ap_get_str_value(parser, "f") : NULL;
    const char *dest_arg = ap_found(parser, "C") ? ap_get_str_value(parser, "C") : NULL;

    if ((create ? 1 : 0) + (extract ? 1 : 0) + (list ? 1 : 0) != 1 || archive_arg == NULL) {
        stdio__printf("tar: specify exactly one of -c/-x/-t, and -f <archive>\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char archive_path[BRUCE_STORAGE_PATH_MAX];
    if (!bnu__resolve_path(archive_arg, archive_path)) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_PATH;
    }

    bruce_result_t result;
    if (create) {
        char *paths = NULL;
        const char **pointers = NULL;
        result = bnu__archive_resolve_entries(parser, 0, ap_count_args(parser), &paths, &pointers);
        int entry_count = ap_count_args(parser);
        ap_free(parser);
        if (result != BRUCE_OK) {
            if (result == BRUCE_ERR_INVALID_ARGUMENT) stdio__printf("tar: missing file operand\n");
            return result;
        }
        result = archive__tar_gz_create(archive_path, pointers, (size_t)entry_count, BRUCE_COMPRESS_LEVEL_DEFAULT);
        bnu__archive_free_entries(paths, pointers);
    } else if (list) {
        ap_free(parser);
        result = archive__tar_gz_list(archive_path, bnu__archive_print_entry, NULL);
    } else {
        char dest_path[BRUCE_STORAGE_PATH_MAX];
        result = bnu__archive_resolve_dest(archive_path, dest_arg, dest_path, sizeof(dest_path));
        ap_free(parser);
        if (result == BRUCE_OK) result = archive__tar_gz_extract(archive_path, dest_path);
    }

    if (result != BRUCE_OK) stdio__printf("tar: %s: %s\n", archive_path, result__to_string(result));
    return result;
}

/* ------------------------------------------------------------------------ */
/* zip                                                                      */
/* ------------------------------------------------------------------------ */

int bnu_zip_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Create a \".zip\" archive from files or directories.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "archive", "Destination \".zip\" path");
    ap_add_required_arg(parser, "file", "File(s)/directory(ies) to add");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int total = ap_count_args(parser);
    if (total < 2) {
        stdio__printf("zip: missing file operand\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    char archive_path[BRUCE_STORAGE_PATH_MAX];
    if (!bnu__resolve_path(ap_get_arg_at_index(parser, 0), archive_path)) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_PATH;
    }

    char *paths = NULL;
    const char **pointers = NULL;
    bruce_result_t result = bnu__archive_resolve_entries(parser, 1, total, &paths, &pointers);
    int entry_count = total - 1;
    ap_free(parser);
    if (result != BRUCE_OK) return result;

    result = archive__zip_create(archive_path, pointers, (size_t)entry_count, BRUCE_COMPRESS_LEVEL_DEFAULT);
    bnu__archive_free_entries(paths, pointers);
    if (result != BRUCE_OK) stdio__printf("zip: %s: %s\n", archive_path, result__to_string(result));
    return result;
}

/* ------------------------------------------------------------------------ */
/* unzip                                                                    */
/* ------------------------------------------------------------------------ */

int bnu_unzip_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("List or extract a \".zip\" archive.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "l");
    ap_set_opt_help(parser, "l", "List contents instead of extracting");
    ap_add_str_opt(parser, "d", NULL);
    ap_set_opt_help(parser, "d", "Directory to extract into (defaults to the archive's own directory)");
    ap_add_required_arg(parser, "archive", "\".zip\" file to read");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool list = ap_found(parser, "l");
    const char *dest_arg = ap_found(parser, "d") ? ap_get_str_value(parser, "d") : NULL;
    char archive_path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "archive"), archive_path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    bruce_result_t result;
    if (list) {
        result = archive__zip_list(archive_path, bnu__archive_print_entry, NULL);
    } else {
        char dest_path[BRUCE_STORAGE_PATH_MAX];
        result = bnu__archive_resolve_dest(archive_path, dest_arg, dest_path, sizeof(dest_path));
        if (result == BRUCE_OK) result = archive__zip_extract(archive_path, dest_path);
    }
    if (result != BRUCE_OK) stdio__printf("unzip: %s: %s\n", archive_path, result__to_string(result));
    return result;
}

/* ------------------------------------------------------------------------ */
/* archive / archive-extract (format-agnostic, extensions.conf-driven)      */
/* ------------------------------------------------------------------------ */

int bnu_archive_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("List a \".zip\" or \".tar.gz\"/\".tgz\" archive's contents.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "archive", "Archive file to list");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    char archive_path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "archive"), archive_path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    bruce_result_t result = bnu__archive_is_zip(archive_path)
                                 ? archive__zip_list(archive_path, bnu__archive_print_entry, NULL)
                                 : archive__tar_gz_list(archive_path, bnu__archive_print_entry, NULL);
    if (result != BRUCE_OK) stdio__printf("archive: %s: %s\n", archive_path, result__to_string(result));
    return result;
}

int bnu_archive_extract_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Extract a \".zip\" or \".tar.gz\"/\".tgz\" archive.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "C", NULL);
    ap_set_opt_help(parser, "C", "Directory to extract into (defaults to the archive's own directory)");
    ap_add_required_arg(parser, "archive", "Archive file to extract");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *dest_arg = ap_found(parser, "C") ? ap_get_str_value(parser, "C") : NULL;
    char archive_path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "archive"), archive_path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    char dest_path[BRUCE_STORAGE_PATH_MAX];
    bruce_result_t result = bnu__archive_resolve_dest(archive_path, dest_arg, dest_path, sizeof(dest_path));
    if (result == BRUCE_OK) {
        result = bnu__archive_is_zip(archive_path) ? archive__zip_extract(archive_path, dest_path)
                                                     : archive__tar_gz_extract(archive_path, dest_path);
    }
    if (result != BRUCE_OK) stdio__printf("archive-extract: %s: %s\n", archive_path, result__to_string(result));
    return result;
}
