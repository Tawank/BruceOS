#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/result.h"

/**
 * @brief ".tar.gz" and ".zip" archive create/list/extract.
 *
 * ".tar.gz": core_sdk/compress.h (the gzip layer) plus a vendored
 * tar-format reader/writer (components/microtar). ".zip": a vendored
 * zip container reader/writer (components/minizip) that talks to zlib
 * directly for its own per-entry deflate, the same way libpng does - it
 * doesn't go through core_sdk/compress.h. Both are wired to Bruce's own
 * storage__ calls instead of stdio - see core/archive/archive.c (tar.gz)
 * and core/archive/archive_zip.c (zip) for the I/O shims. Used by the
 * `tar`/`zip`/`archive`/`archive-extract` bnu commands and filemanager's
 * "Extract here" action.
 *
 * Creating a ".tar.gz" is a single streaming pass (tar formatting -> gzip
 * compression -> storage__write), never holding more than one I/O chunk in
 * memory regardless of archive size. Creating/reading a ".zip" streams
 * each entry independently the same way, straight against storage__ -
 * a zip's central directory supports real random access (see
 * archive__zip_create()'s doc comment), so unlike ".tar.gz" nothing extra
 * is needed to read one back.
 *
 * Listing/extracting a ".tar.gz" is the one exception: gzip decompression
 * is inherently forward-only (there's no seeking backward in a deflate
 * stream without re-decompressing from the start), but the tar reader
 * needs to seek between entries. So both first fully decompress the
 * archive to a same-directory scratch ".tar" file, read/extract from that
 * with ordinary seeks, then delete the scratch file - meaning free storage
 * space at least equal to the decompressed archive size is needed
 * temporarily, on top of the archive itself. Fine for the
 * sprite/config/small-archive sizes this targets; worth knowing before
 * pointing this at something huge.
 */

#define BRUCE_ARCHIVE_ENTRY_NAME_MAX 100

typedef struct {
    /* Entry's path within the archive, e.g. "photos/cat.jpg". Always
     * forward-slash separated regardless of host path conventions. */
    char name[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
    /* Uncompressed size in bytes; 0 for directories. */
    size_t size;
    bool is_directory;
} bruce_archive_entry_t;

/**
 * @brief Called once per archive entry by archive__tar_gz_list().
 *
 * @param context Caller-supplied pointer, passed through unchanged.
 * @param entry The entry just read.
 * @return false to stop listing early (e.g. caller found what it needed); true to continue.
 */
typedef bool (*bruce_archive_list_fn)(void *context, const bruce_archive_entry_t *entry);

/**
 * @brief Creates a new ".tar.gz" archive from a set of existing paths.
 *
 * Each of `entry_paths` is added under its own basename; a directory is
 * added recursively (its own name becomes an archive-relative root, e.g.
 * adding "/sd/photos" produces entries named "photos/", "photos/a.jpg", ...).
 * `archive_path` is truncated/created fresh - any existing file there is
 * overwritten only once the whole archive has been built successfully
 * (nothing already at `archive_path` is touched if this fails partway).
 *
 * @param archive_path Destination ".tar.gz" path.
 * @param entry_paths Existing file/directory paths to add.
 * @param entry_count Number of paths in entry_paths.
 * @param level BRUCE_COMPRESS_LEVEL_DEFAULT, or 0-9 (see core_sdk/compress.h).
 * @permission storage
 */
bruce_result_t
archive__tar_gz_create(const char *archive_path, const char *const *entry_paths, size_t entry_count, int level);

/**
 * @brief Lists a ".tar.gz" archive's entries without extracting them.
 *
 * @param archive_path Archive to list.
 * @param callback Called once per entry, in archive order.
 * @param context Passed through to every callback call unchanged.
 * @permission storage
 */
bruce_result_t archive__tar_gz_list(const char *archive_path, bruce_archive_list_fn callback, void *context);

/**
 * @brief Extracts every entry of a ".tar.gz" archive under `dest_dir`.
 *
 * `dest_dir` must already exist. Entry names are validated before anything
 * is written: an absolute name or one containing a ".." component is
 * rejected (BRUCE_ERR_INVALID_PATH) rather than allowed to write outside
 * `dest_dir` ("tar-slip"). Missing intermediate directories under
 * `dest_dir` are created as needed.
 *
 * @param archive_path Archive to extract.
 * @param dest_dir Existing directory to extract into.
 * @permission storage
 */
bruce_result_t archive__tar_gz_extract(const char *archive_path, const char *dest_dir);

/**
 * @brief Creates a new ".zip" archive from a set of existing paths.
 *
 * Same entry-naming/recursion rules as archive__tar_gz_create(). Unlike
 * ".tar.gz", a ".zip"'s entries are compressed individually and its
 * central directory supports real random access, so - unlike
 * archive__tar_gz_list()/_extract() - no scratch decompression step is
 * needed for reading one back; the underlying storage__seek() is used
 * directly.
 *
 * @param archive_path Destination ".zip" path.
 * @param entry_paths Existing file/directory paths to add.
 * @param entry_count Number of paths in entry_paths.
 * @param level BRUCE_COMPRESS_LEVEL_DEFAULT, or 0-9 (see core_sdk/compress.h).
 * @permission storage
 */
bruce_result_t
archive__zip_create(const char *archive_path, const char *const *entry_paths, size_t entry_count, int level);

/**
 * @brief Lists a ".zip" archive's entries without extracting them.
 *
 * @param archive_path Archive to list.
 * @param callback Called once per entry, in archive order.
 * @param context Passed through to every callback call unchanged.
 * @permission storage
 */
bruce_result_t archive__zip_list(const char *archive_path, bruce_archive_list_fn callback, void *context);

/**
 * @brief Extracts every entry of a ".zip" archive under `dest_dir`.
 *
 * Same "dest_dir must exist"/"zip-slip"-rejection/intermediate-directory
 * rules as archive__tar_gz_extract().
 *
 * @param archive_path Archive to extract.
 * @param dest_dir Existing directory to extract into.
 * @permission storage
 */
bruce_result_t archive__zip_extract(const char *archive_path, const char *dest_dir);
