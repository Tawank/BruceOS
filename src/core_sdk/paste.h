#pragma once

#include <stddef.h>

#include "core_sdk/result.h"

/**
 * @brief Shared copy/paste clipboard.
 *
 * A single system-wide clipboard slot, independent of any process's
 * lifetime: one app (e.g. the file manager) can copy something and a
 * different app -- launched later, by a different process, possibly after
 * the copying process has already exited -- can paste it (e.g. the text
 * editor, or the file manager again in a different directory). Setting
 * either kind of content replaces whatever was there before; there is
 * exactly one slot, no history and no separate text/file buffers.
 */

#define BRUCE_PASTE_MAX_FILES 64

typedef enum {
    BRUCE_PASTE_EMPTY = 0,
    BRUCE_PASTE_TEXT,
    BRUCE_PASTE_FILES,
} bruce_paste_kind_t;

typedef enum {
    BRUCE_PASTE_FILE_COPY = 0,
    BRUCE_PASTE_FILE_CUT,
} bruce_paste_file_mode_t;

/** @brief What's currently on the clipboard. */
bruce_paste_kind_t paste__kind(void);

/**
 * @brief Copies text onto the clipboard, replacing any prior content.
 *
 * @param text Null-terminated text to store; copied internally.
 */
bruce_result_t paste__set_text(const char *text);

/**
 * @brief Returns the clipboard's text, or NULL if paste__kind() is not BRUCE_PASTE_TEXT.
 *
 * The returned pointer is borrowed: valid until the next paste__set_text(),
 * paste__set_files(), or paste__clear() call from any process.
 */
const char *paste__get_text(void);

/**
 * @brief Records file/directory paths on the clipboard for a later paste__paste_files(), replacing any prior content.
 *
 * @param paths Absolute source paths; copied internally.
 * @param count Number of paths, from 1 to BRUCE_PASTE_MAX_FILES.
 * @param mode Whether the eventual paste should copy (leaving the
 *             originals in place) or cut (remove each original once it, and
 *             everything under it, has been pasted successfully).
 */
bruce_result_t paste__set_files(const char *const *paths, size_t count, bruce_paste_file_mode_t mode);

/** @brief Number of paths on the clipboard (0 unless paste__kind() is BRUCE_PASTE_FILES). */
size_t paste__file_count(void);

/**
 * @brief Returns one clipboard path by index, or NULL if index is out of range.
 *
 * The returned pointer is borrowed under the same rules as paste__get_text().
 *
 * @param index Zero-based index, below paste__file_count().
 */
const char *paste__get_file(size_t index);

/** @brief Whether the clipboard's files were cut or copied; meaningless when paste__file_count() is 0. */
bruce_paste_file_mode_t paste__file_mode(void);

/**
 * @brief Copies (or moves, for a cut) every clipboard file/directory into target_directory.
 *
 * Recurses into directories, creating them at the destination as needed.
 * Refuses (BRUCE_ERR_ALREADY_EXISTS) rather than overwriting any
 * destination path that already exists, and refuses
 * (BRUCE_ERR_INVALID_ARGUMENT) a target that is one of the clipboard's own
 * directories or nested inside one, rather than recursing into its own
 * output forever. Stops at the first failure; a cut has already removed
 * whichever earlier entries it fully copied, but does not roll them back.
 * The clipboard itself is left untouched either way, so the same content
 * can be pasted again.
 *
 * @param target_directory Absolute destination directory; must already exist.
 * @permission storage
 */
bruce_result_t paste__paste_files(const char *target_directory);

/** @brief Clears the clipboard; paste__kind() becomes BRUCE_PASTE_EMPTY. */
void paste__clear(void);
