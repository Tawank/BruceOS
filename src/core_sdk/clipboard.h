#pragma once

#include <stdbool.h>
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
 * exactly one slot, no history and no separate text/file/binary buffers.
 */

#define BRUCE_CLIPBOARD_MAX_FILES 64

/* Headroom over the largest known binary producer (the browser's fetched-
 * image cache, capped at 256 KiB -- see BROWSER_IMAGE_MAX_BYTES), since
 * unlike a per-request buffer this one is held for as long as it's on the
 * clipboard, of unpredictable duration. */
#define BRUCE_CLIPBOARD_MAX_BINARY_BYTES (512u * 1024u)

typedef enum {
    BRUCE_CLIPBOARD_EMPTY = 0,
    BRUCE_CLIPBOARD_TEXT,
    BRUCE_CLIPBOARD_FILES,
    /* Raw bytes with no filesystem home of their own yet, e.g. an image
     * fetched by the browser but never saved to storage -- see
     * clipboard__set_binary(). */
    BRUCE_CLIPBOARD_BINARY,
} bruce_clipboard_kind_t;

typedef enum {
    BRUCE_CLIPBOARD_FILE_COPY = 0,
    BRUCE_CLIPBOARD_FILE_CUT,
} bruce_clipboard_file_mode_t;

/** @brief What's currently on the clipboard. */
bruce_clipboard_kind_t clipboard__kind(void);

/**
 * @brief Copies text onto the clipboard, replacing any prior content.
 *
 * @param text Null-terminated text to store; copied internally.
 */
bruce_result_t clipboard__set_text(const char *text);

/**
 * @brief Returns the clipboard's text, or NULL if clipboard__kind() is not BRUCE_CLIPBOARD_TEXT.
 *
 * The returned pointer is borrowed: valid until the next clipboard__set_text(),
 * clipboard__set_files(), or clipboard__clear() call from any process.
 */
const char *clipboard__get_text(void);

/**
 * @brief Records file/directory paths on the clipboard for a later clipboard__paste_files(), replacing any prior content.
 *
 * @param paths Absolute source paths; copied internally.
 * @param count Number of paths, from 1 to BRUCE_CLIPBOARD_MAX_FILES.
 * @param mode Whether the eventual paste should copy (leaving the
 *             originals in place) or cut (remove each original once it, and
 *             everything under it, has been pasted successfully).
 */
bruce_result_t clipboard__set_files(const char *const *paths, size_t count, bruce_clipboard_file_mode_t mode);

/** @brief Number of paths on the clipboard (0 unless clipboard__kind() is BRUCE_CLIPBOARD_FILES). */
size_t clipboard__file_count(void);

/**
 * @brief Returns one clipboard path by index, or NULL if index is out of range.
 *
 * The returned pointer is borrowed under the same rules as clipboard__get_text().
 *
 * @param index Zero-based index, below clipboard__file_count().
 */
const char *clipboard__get_file(size_t index);

/** @brief Whether the clipboard's files were cut or copied; meaningless when clipboard__file_count() is 0. */
bruce_clipboard_file_mode_t clipboard__file_mode(void);

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
bruce_result_t clipboard__paste_files(const char *target_directory);

/**
 * @brief Copies/moves a single clipboard entry to an exact destination path.
 *
 * Same recursive copy/move engine as clipboard__paste_files(), but for one
 * clipboard entry chosen by index and a caller-supplied destination path
 * (rather than target_directory + the source's own basename) - lets a
 * caller (e.g. the file manager) paste a copy under a different name, or
 * explicitly allow overwriting an existing destination instead of getting
 * BRUCE_ERR_ALREADY_EXISTS back. Refuses (BRUCE_ERR_INVALID_ARGUMENT) a
 * target_path equal to the source itself, or one of the source's own
 * directories or nested inside one, same as clipboard__paste_files() does
 * for its target_directory.
 *
 * @param index Zero-based clipboard file index, below clipboard__file_count().
 * @param target_path Absolute destination path, including the final name.
 * @param overwrite When false, refuses (BRUCE_ERR_ALREADY_EXISTS) an
 *                   existing destination, matching clipboard__paste_files();
 *                   when true, removes it (file, or whole directory tree)
 *                   first.
 * @permission storage
 */
bruce_result_t clipboard__paste_file_as(size_t index, const char *target_path, bool overwrite);

/**
 * @brief Copies raw bytes onto the clipboard, replacing any prior content.
 *
 * For content that has no filesystem path of its own (yet) to hand to
 * clipboard__set_files() instead -- e.g. an image the browser fetched but
 * the user hasn't chosen to save anywhere.
 *
 * @param data Bytes to store; copied internally.
 * @param len Length of data in bytes, from 1 to BRUCE_CLIPBOARD_MAX_BINARY_BYTES.
 * @param filename Optional suggested filename (a bare name, no directory
 *                 separators) for whatever later writes these bytes out --
 *                 see clipboard__binary_filename() -- or NULL to leave it
 *                 unset.
 */
bruce_result_t clipboard__set_binary(const void *data, size_t len, const char *filename);

/** @brief Size in bytes of the clipboard's binary payload (0 unless clipboard__kind() is BRUCE_CLIPBOARD_BINARY). */
size_t clipboard__binary_size(void);

/**
 * @brief Returns the clipboard's binary payload, or NULL if clipboard__kind() is not BRUCE_CLIPBOARD_BINARY.
 *
 * The returned pointer is borrowed under the same rules as clipboard__get_text().
 */
const void *clipboard__get_binary(void);

/**
 * @brief The binary payload's suggested filename, or NULL if none was given.
 *
 * Meaningless when clipboard__kind() is not BRUCE_CLIPBOARD_BINARY. A
 * pasting app (e.g. the file manager) should fall back to asking the user
 * for a name when this is NULL -- some copier may not have one to offer.
 */
const char *clipboard__binary_filename(void);

/**
 * @brief Writes the clipboard's binary payload to target_path, creating it.
 *
 * Refuses (BRUCE_ERR_ALREADY_EXISTS) rather than overwriting an existing
 * file there, matching clipboard__paste_files(). Reports
 * BRUCE_ERR_INVALID_STATE if clipboard__kind() is not
 * BRUCE_CLIPBOARD_BINARY. The clipboard itself is left untouched either
 * way, so the same content can be pasted again.
 *
 * @param target_path Absolute destination file path; must not already exist.
 * @permission storage
 */
bruce_result_t clipboard__paste_binary(const char *target_path);

/** @brief Clears the clipboard; clipboard__kind() becomes BRUCE_CLIPBOARD_EMPTY. */
void clipboard__clear(void);
