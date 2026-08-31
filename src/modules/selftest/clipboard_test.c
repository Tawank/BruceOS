#include "clipboard_test.h"

#include "core_sdk/clipboard.h"
#include "core_sdk/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A few paths below are built by snprintf()-ing a suffix onto a directory
 * path that was itself just snprintf()'d into a BRUCE_STORAGE_PATH_MAX
 * buffer. The actual strings involved here are always short, but GCC's
 * -Werror=format-truncation only sees that source buffer's declared
 * capacity (it doesn't trace what an earlier snprintf() actually wrote into
 * it), so it assumes a worst-case near-full path and flags the
 * concatenation as possibly truncating. Giving the destination a little
 * slack over BRUCE_STORAGE_PATH_MAX -- enough for the longest suffix used
 * below, "/tree/inner.txt" -- makes the bound provably safe instead of just
 * actually safe. */
#define SELFTEST_CLIPBOARD_PATH_MAX (BRUCE_STORAGE_PATH_MAX + 16)

/* Recursively removes `path` (file or directory, present or not) so each
 * case starts from a clean slate and leaves none of its scratch files
 * behind - storage__remove() itself only accepts an already-empty
 * directory. */
static void selftest__clipboard_remove_tree(const char *path) {
    size_t count = 0;
    if (storage__list(path, NULL, 0, &count) == BRUCE_OK) {
        bruce_storage_entry_t *entries = count > 0 ? malloc(count * sizeof(*entries)) : NULL;
        if (entries != NULL && storage__list(path, entries, count, &count) == BRUCE_OK) {
            for (size_t i = 0; i < count; ++i) {
                char child[BRUCE_STORAGE_PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, entries[i].name);
                selftest__clipboard_remove_tree(child);
            }
        }
        free(entries);
    }
    (void)storage__remove(path);
}

static bruce_result_t selftest__clipboard_write_file(const char *path, const char *text) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) return result;
    size_t written = 0;
    result = storage__write(file, text, strlen(text), &written);
    bruce_result_t close_result = storage__close(file);
    if (result != BRUCE_OK) return result;
    if (written != strlen(text)) return BRUCE_ERR_IO;
    return close_result;
}

static bool selftest__clipboard_file_contains(const char *path, const char *expected) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return false;
    char buffer[64] = {0};
    size_t read_size = 0;
    bruce_result_t result = storage__read(file, buffer, sizeof(buffer) - 1, &read_size);
    (void)storage__close(file);
    return result == BRUCE_OK && read_size == strlen(expected) && strncmp(buffer, expected, read_size) == 0;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_clipboard_text_case                                        */
/* ------------------------------------------------------------------------ */

bool selftest__run_clipboard_text_case(void) {
    clipboard__clear();
    bool empty_ok =
        clipboard__kind() == BRUCE_CLIPBOARD_EMPTY && clipboard__get_text() == NULL && clipboard__file_count() == 0;

    bruce_result_t null_text_result = clipboard__set_text(NULL);
    bool set_text_ok = clipboard__set_text("hello clipboard") == BRUCE_OK;
    const char *text = clipboard__get_text();
    bool get_text_ok =
        clipboard__kind() == BRUCE_CLIPBOARD_TEXT && text != NULL && strcmp(text, "hello clipboard") == 0;

    const char *bad_paths[] = {"relative/path"};
    bruce_result_t relative_path_result = clipboard__set_files(bad_paths, 1, BRUCE_CLIPBOARD_FILE_COPY);
    bruce_result_t zero_count_result = clipboard__set_files(bad_paths, 0, BRUCE_CLIPBOARD_FILE_COPY);

    /* Setting files replaces the earlier text -- exactly one clipboard slot. */
    const char *file_paths[] = {"/selftest_clipboard_text_probe"};
    bool set_files_ok = clipboard__set_files(file_paths, 1, BRUCE_CLIPBOARD_FILE_CUT) == BRUCE_OK;
    bool replaced_ok = clipboard__kind() == BRUCE_CLIPBOARD_FILES && clipboard__get_text() == NULL &&
                        clipboard__file_count() == 1 && clipboard__file_mode() == BRUCE_CLIPBOARD_FILE_CUT &&
                        clipboard__get_file(0) != NULL && strcmp(clipboard__get_file(0), file_paths[0]) == 0 &&
                        clipboard__get_file(1) == NULL;

    clipboard__clear();
    bool cleared_ok = clipboard__kind() == BRUCE_CLIPBOARD_EMPTY;

    bool ok = empty_ok && null_text_result == BRUCE_ERR_INVALID_ARGUMENT && set_text_ok && get_text_ok &&
              relative_path_result == BRUCE_ERR_INVALID_ARGUMENT &&
              zero_count_result == BRUCE_ERR_INVALID_ARGUMENT && set_files_ok && replaced_ok && cleared_ok;
    printf("[selftest] clipboard/text: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_clipboard_files_case                                       */
/* ------------------------------------------------------------------------ */

bool selftest__run_clipboard_files_case(void) {
    static const char *const root = "/selftest_clipboard";
    selftest__clipboard_remove_tree(root);
    clipboard__clear();

    bool setup_ok = storage__mkdir(root) == BRUCE_OK;
    char source_file[BRUCE_STORAGE_PATH_MAX];
    snprintf(source_file, sizeof(source_file), "%s/src.txt", root);
    setup_ok = setup_ok && selftest__clipboard_write_file(source_file, "copy me") == BRUCE_OK;
    char dest_dir[BRUCE_STORAGE_PATH_MAX];
    snprintf(dest_dir, sizeof(dest_dir), "%s/dst", root);
    setup_ok = setup_ok && storage__mkdir(dest_dir) == BRUCE_OK;

    /* Plain file copy: source is untouched, destination gets a byte-for-byte
     * copy under its original name. */
    const char *copy_sources[] = {source_file};
    bruce_result_t copy_set = clipboard__set_files(copy_sources, 1, BRUCE_CLIPBOARD_FILE_COPY);
    bruce_result_t copy_result = clipboard__paste_files(dest_dir);
    char copied_file[SELFTEST_CLIPBOARD_PATH_MAX];
    snprintf(copied_file, sizeof(copied_file), "%s/src.txt", dest_dir);
    bool source_survived = false;
    bruce_result_t source_exists_result = storage__exists(source_file, &source_survived);
    bool copy_ok = copy_set == BRUCE_OK && copy_result == BRUCE_OK && source_exists_result == BRUCE_OK &&
                   source_survived && selftest__clipboard_file_contains(copied_file, "copy me");

    /* Pasting the same clipboard again refuses to clobber the existing copy. */
    bruce_result_t duplicate_result = clipboard__paste_files(dest_dir);
    bool duplicate_ok = duplicate_result == BRUCE_ERR_ALREADY_EXISTS;

    /* clipboard__paste_file_as() can do what clipboard__paste_files() just
     * refused above, once asked to: overwrite the existing copy in place. */
    bruce_result_t overwrite_setup_ok = selftest__clipboard_write_file(source_file, "overwritten");
    bruce_result_t overwrite_result = clipboard__paste_file_as(0, copied_file, true);
    bool overwrite_ok = overwrite_setup_ok == BRUCE_OK && overwrite_result == BRUCE_OK &&
                        selftest__clipboard_file_contains(copied_file, "overwritten");

    /* Without overwrite, it refuses an existing destination the same as
     * clipboard__paste_files() does. */
    bruce_result_t no_overwrite_result = clipboard__paste_file_as(0, copied_file, false);
    bool no_overwrite_ok = no_overwrite_result == BRUCE_ERR_ALREADY_EXISTS;

    /* A target_path equal to the source itself is refused outright, even
     * with overwrite requested - it would otherwise delete the source before
     * "copying" it back onto itself. */
    bruce_result_t self_target_result = clipboard__paste_file_as(0, source_file, true);
    bool self_target_ok = self_target_result == BRUCE_ERR_INVALID_ARGUMENT;

    /* A caller-chosen destination name/path, not just target_directory +
     * the source's own basename. */
    char renamed_file[SELFTEST_CLIPBOARD_PATH_MAX];
    snprintf(renamed_file, sizeof(renamed_file), "%s/renamed.txt", dest_dir);
    bruce_result_t rename_paste_result = clipboard__paste_file_as(0, renamed_file, false);
    bool rename_paste_ok =
        rename_paste_result == BRUCE_OK && selftest__clipboard_file_contains(renamed_file, "overwritten");

    /* Recursive directory copy: a nested file comes along with its folder. */
    char source_dir[BRUCE_STORAGE_PATH_MAX];
    snprintf(source_dir, sizeof(source_dir), "%s/tree", root);
    char nested_file[SELFTEST_CLIPBOARD_PATH_MAX];
    snprintf(nested_file, sizeof(nested_file), "%s/inner.txt", source_dir);
    bool tree_setup_ok = storage__mkdir(source_dir) == BRUCE_OK &&
                         selftest__clipboard_write_file(nested_file, "nested") == BRUCE_OK;
    const char *tree_sources[] = {source_dir};
    bruce_result_t tree_set = clipboard__set_files(tree_sources, 1, BRUCE_CLIPBOARD_FILE_COPY);
    bruce_result_t tree_result = clipboard__paste_files(dest_dir);
    char copied_nested_file[SELFTEST_CLIPBOARD_PATH_MAX];
    snprintf(copied_nested_file, sizeof(copied_nested_file), "%s/tree/inner.txt", dest_dir);
    bool tree_ok = tree_setup_ok && tree_set == BRUCE_OK && tree_result == BRUCE_OK &&
                   selftest__clipboard_file_contains(copied_nested_file, "nested");

    /* Pasting a directory into itself is refused instead of recursing into
     * its own output forever. The clipboard still holds `source_dir` from
     * the copy just above. */
    bruce_result_t self_paste_result = clipboard__paste_files(source_dir);

    /* Cut removes the source only once the destination copy has fully
     * succeeded. */
    char cut_source[BRUCE_STORAGE_PATH_MAX];
    snprintf(cut_source, sizeof(cut_source), "%s/cut.txt", root);
    char cut_dest_dir[BRUCE_STORAGE_PATH_MAX];
    snprintf(cut_dest_dir, sizeof(cut_dest_dir), "%s/dst2", root);
    bool cut_setup_ok = selftest__clipboard_write_file(cut_source, "move me") == BRUCE_OK &&
                        storage__mkdir(cut_dest_dir) == BRUCE_OK;
    const char *cut_sources[] = {cut_source};
    bruce_result_t cut_set = clipboard__set_files(cut_sources, 1, BRUCE_CLIPBOARD_FILE_CUT);
    bruce_result_t cut_result = clipboard__paste_files(cut_dest_dir);
    char cut_destination_file[SELFTEST_CLIPBOARD_PATH_MAX];
    snprintf(cut_destination_file, sizeof(cut_destination_file), "%s/cut.txt", cut_dest_dir);
    bool cut_source_exists = true;
    bruce_result_t cut_source_check = storage__exists(cut_source, &cut_source_exists);
    bool cut_ok = cut_setup_ok && cut_set == BRUCE_OK && cut_result == BRUCE_OK &&
                  cut_source_check == BRUCE_OK && !cut_source_exists &&
                  selftest__clipboard_file_contains(cut_destination_file, "move me");

    /* An empty clipboard, and a target directory that doesn't exist, both
     * fail without touching anything. */
    clipboard__clear();
    bruce_result_t empty_clipboard_result = clipboard__paste_files(dest_dir);
    const char *missing_sources[] = {source_file};
    (void)clipboard__set_files(missing_sources, 1, BRUCE_CLIPBOARD_FILE_COPY);
    bruce_result_t missing_target_result = clipboard__paste_files("/selftest_clipboard_missing_target");

    selftest__clipboard_remove_tree(root);
    clipboard__clear();

    bool ok = setup_ok && copy_ok && duplicate_ok && overwrite_ok && no_overwrite_ok && self_target_ok &&
              rename_paste_ok && tree_ok && self_paste_result == BRUCE_ERR_INVALID_ARGUMENT && cut_ok &&
              empty_clipboard_result == BRUCE_ERR_INVALID_STATE &&
              missing_target_result == BRUCE_ERR_NOT_FOUND;
    printf(
        "[selftest] clipboard/files: %s (copy=%d dup=%d overwrite=%d no_overwrite=%d self_target=%d "
        "rename=%d tree=%d self=%d cut=%d empty=%d missing=%d)\n",
        ok ? "OK" : "FAIL",
        copy_result,
        duplicate_result,
        overwrite_result,
        no_overwrite_result,
        self_target_result,
        rename_paste_result,
        tree_result,
        self_paste_result,
        cut_result,
        empty_clipboard_result,
        missing_target_result
    );
    return ok;
}
