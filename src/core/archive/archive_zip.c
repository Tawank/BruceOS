#include "core_sdk/archive.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unzip.h>
#include <zip.h>

#include "core_sdk/compress.h"
#include "core_sdk/storage.h"

/*
 * ".zip" support. Talks to zlib directly for its own per-entry deflate
 * (like libpng does), the same way the vendored minizip (zip.c/unzip.c)
 * always has - core_sdk/compress.h isn't involved here, see
 * core_sdk/archive.h's module doc comment. What this file adds is a
 * storage-backed zlib_filefunc64_def (see ioapi.h) so minizip reads/writes
 * through storage__* instead of stdio's FILE*.
 */

#define ARCHIVE_ZIP_CHUNK_SIZE 512

typedef struct {
    bruce_file_id_t file;
} archive__zip_stream_t;

static voidpf archive__zip_open_cb(voidpf opaque, const void *filename, int mode) {
    (void)opaque;
    uint32_t flags = 0;
    if (mode & ZLIB_FILEFUNC_MODE_READ) flags |= BRUCE_STORAGE_OPEN_READ;
    if (mode & ZLIB_FILEFUNC_MODE_WRITE) flags |= BRUCE_STORAGE_OPEN_WRITE;
    if (mode & ZLIB_FILEFUNC_MODE_CREATE) flags |= BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE;

    archive__zip_stream_t *stream = malloc(sizeof(*stream));
    if (stream == NULL) return NULL;
    if (storage__open((const char *)filename, flags, &stream->file) != BRUCE_OK) {
        free(stream);
        return NULL;
    }
    return stream;
}

static uLong archive__zip_read_cb(voidpf opaque, voidpf handle, void *buf, uLong size) {
    (void)opaque;
    archive__zip_stream_t *stream = (archive__zip_stream_t *)handle;
    size_t read_size = 0;
    return storage__read(stream->file, buf, size, &read_size) == BRUCE_OK ? (uLong)read_size : 0;
}

static uLong archive__zip_write_cb(voidpf opaque, voidpf handle, const void *buf, uLong size) {
    (void)opaque;
    archive__zip_stream_t *stream = (archive__zip_stream_t *)handle;
    size_t written = 0;
    return storage__write(stream->file, buf, size, &written) == BRUCE_OK ? (uLong)written : 0;
}

static ZPOS64_T archive__zip_tell_cb(voidpf opaque, voidpf handle) {
    (void)opaque;
    archive__zip_stream_t *stream = (archive__zip_stream_t *)handle;
    uint64_t position = 0;
    return storage__seek(stream->file, 0, SEEK_CUR, &position) == BRUCE_OK ? (ZPOS64_T)position : (ZPOS64_T)-1;
}

static long archive__zip_seek_cb(voidpf opaque, voidpf handle, ZPOS64_T offset, int origin) {
    (void)opaque;
    archive__zip_stream_t *stream = (archive__zip_stream_t *)handle;
    int whence =
        origin == ZLIB_FILEFUNC_SEEK_CUR ? SEEK_CUR : origin == ZLIB_FILEFUNC_SEEK_END ? SEEK_END : SEEK_SET;
    return storage__seek(stream->file, (int64_t)offset, whence, NULL) == BRUCE_OK ? 0 : -1;
}

static int archive__zip_close_cb(voidpf opaque, voidpf handle) {
    (void)opaque;
    archive__zip_stream_t *stream = (archive__zip_stream_t *)handle;
    bruce_result_t result = storage__close(stream->file);
    free(stream);
    return result == BRUCE_OK ? 0 : -1;
}

static int archive__zip_error_cb(voidpf opaque, voidpf handle) {
    (void)opaque;
    (void)handle;
    return 0; /* errors already surface as short read__cb/write_cb counts */
}

static void archive__zip_filefunc(zlib_filefunc64_def *out) {
    memset(out, 0, sizeof(*out));
    out->zopen64_file = archive__zip_open_cb;
    out->zread_file = archive__zip_read_cb;
    out->zwrite_file = archive__zip_write_cb;
    out->ztell64_file = archive__zip_tell_cb;
    out->zseek64_file = archive__zip_seek_cb;
    out->zclose_file = archive__zip_close_cb;
    out->zerror_file = archive__zip_error_cb;
}

/* ---- create ---- */

static const char *archive__zip_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bruce_result_t archive__zip_add_path(zipFile zf, const char *fs_path, const char *archive_name, int level);

/* zipOpenNewFileInZip() (minizip's own convenience wrapper) hardcodes
 * deflateInit2()'s windowBits/memLevel at zlib's own defaults (-15/8, ~256KB
 * combined - see zlib.h), which can fail with Z_MEM_ERROR on a fragmented
 * embedded heap even though compress__start() (core/compress/compress.c)
 * already works around the exact same problem for the gzip path - minizip
 * talks to zlib directly (see this file's module doc comment) so that fix
 * doesn't cover it. Step window size and memLevel down the same way here,
 * via zipOpenNewFileInZip3()'s explicit parameters instead of the wrapper. */
static int archive__zip_open_new_file(zipFile zf, const char *archive_name, const zip_fileinfo *info, int level) {
    static const int WINDOW_BITS[] = {-15, -12, -10, -9};
    static const int MEM_LEVELS[] = {8, 6, 4, 1};
    int rc = Z_MEM_ERROR;
    for (size_t i = 0; i < sizeof(WINDOW_BITS) / sizeof(WINDOW_BITS[0]); ++i) {
        rc = zipOpenNewFileInZip3(
            zf, archive_name, info, NULL, 0, NULL, 0, NULL, Z_DEFLATED, level, 0, WINDOW_BITS[i], MEM_LEVELS[i],
            Z_DEFAULT_STRATEGY, NULL, 0
        );
        if (rc == ZIP_OK || rc != Z_MEM_ERROR) break;
    }
    return rc;
}

static bruce_result_t archive__zip_add_file(zipFile zf, const char *fs_path, const char *archive_name, int level) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(fs_path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    zip_fileinfo info;
    memset(&info, 0, sizeof(info));
    int compression_level = level == BRUCE_COMPRESS_LEVEL_DEFAULT ? Z_DEFAULT_COMPRESSION : level;
    if (archive__zip_open_new_file(zf, archive_name, &info, compression_level) != ZIP_OK) {
        (void)storage__close(file);
        return BRUCE_ERR_IO;
    }

    uint8_t chunk[ARCHIVE_ZIP_CHUNK_SIZE];
    for (;;) {
        size_t read_size = 0;
        result = storage__read(file, chunk, sizeof(chunk), &read_size);
        if (result != BRUCE_OK) break;
        if (read_size == 0) break;
        if (zipWriteInFileInZip(zf, chunk, (unsigned)read_size) != ZIP_OK) {
            result = BRUCE_ERR_IO;
            break;
        }
    }
    (void)storage__close(file);
    if (zipCloseFileInZip(zf) != ZIP_OK && result == BRUCE_OK) result = BRUCE_ERR_IO;
    return result;
}

static bruce_result_t archive__zip_add_directory(zipFile zf, const char *fs_path, const char *archive_name, int level) {
    char dir_name[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
    int name_written = snprintf(dir_name, sizeof(dir_name), "%s/", archive_name);
    if (name_written < 0 || (size_t)name_written >= sizeof(dir_name)) return BRUCE_ERR_RESOURCE_LIMIT;

    zip_fileinfo info;
    memset(&info, 0, sizeof(info));
    if (archive__zip_open_new_file(zf, dir_name, &info, Z_DEFAULT_COMPRESSION) != ZIP_OK) return BRUCE_ERR_IO;
    if (zipCloseFileInZip(zf) != ZIP_OK) return BRUCE_ERR_IO;

    size_t count = 0;
    bruce_result_t result = storage__list(fs_path, NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    if (count == 0) return BRUCE_OK;

    bruce_storage_entry_t *entries = malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t actual = 0;
    result = storage__list(fs_path, entries, count, &actual);
    if (result != BRUCE_OK) {
        free(entries);
        return result;
    }

    for (size_t i = 0; i < actual && result == BRUCE_OK; ++i) {
        char child_fs_path[BRUCE_STORAGE_PATH_MAX];
        char child_archive_name[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
        int fs_written = snprintf(child_fs_path, sizeof(child_fs_path), "%s/%s", fs_path, entries[i].name);
        int name_written2 =
            snprintf(child_archive_name, sizeof(child_archive_name), "%s/%s", archive_name, entries[i].name);
        if (fs_written < 0 || (size_t)fs_written >= sizeof(child_fs_path) || name_written2 < 0 ||
            (size_t)name_written2 >= sizeof(child_archive_name)) {
            result = BRUCE_ERR_RESOURCE_LIMIT;
            break;
        }
        result = archive__zip_add_path(zf, child_fs_path, child_archive_name, level);
    }
    free(entries);
    return result;
}

static bruce_result_t archive__zip_add_path(zipFile zf, const char *fs_path, const char *archive_name, int level) {
    size_t probe_count = 0;
    bool is_directory = storage__list(fs_path, NULL, 0, &probe_count) == BRUCE_OK;
    return is_directory ? archive__zip_add_directory(zf, fs_path, archive_name, level)
                         : archive__zip_add_file(zf, fs_path, archive_name, level);
}

bruce_result_t
archive__zip_create(const char *archive_path, const char *const *entry_paths, size_t entry_count, int level) {
    if (archive_path == NULL || (entry_paths == NULL && entry_count > 0)) return BRUCE_ERR_INVALID_ARGUMENT;

    zlib_filefunc64_def filefunc;
    archive__zip_filefunc(&filefunc);
    zipFile zf = zipOpen2_64(archive_path, APPEND_STATUS_CREATE, NULL, &filefunc);
    if (zf == NULL) return BRUCE_ERR_IO;

    bruce_result_t result = BRUCE_OK;
    for (size_t i = 0; i < entry_count && result == BRUCE_OK; ++i) {
        result = archive__zip_add_path(zf, entry_paths[i], archive__zip_basename(entry_paths[i]), level);
    }

    int close_result = zipClose(zf, NULL);
    if (result == BRUCE_OK && close_result != ZIP_OK) result = BRUCE_ERR_IO;
    if (result != BRUCE_OK) (void)storage__remove(archive_path);
    return result;
}

/* ---- list ---- */

bruce_result_t archive__zip_list(const char *archive_path, bruce_archive_list_fn callback, void *context) {
    if (archive_path == NULL || callback == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    zlib_filefunc64_def filefunc;
    archive__zip_filefunc(&filefunc);
    unzFile uf = unzOpen2_64(archive_path, &filefunc);
    if (uf == NULL) return BRUCE_ERR_NOT_FOUND;

    bruce_result_t result = BRUCE_OK;
    int go_result = unzGoToFirstFile(uf);
    while (go_result == UNZ_OK) {
        unz_file_info64 info;
        char name[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
        if (unzGetCurrentFileInfo64(uf, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK) {
            result = BRUCE_ERR_IO;
            break;
        }

        bruce_archive_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        strncpy(entry.name, name, sizeof(entry.name) - 1);
        entry.size = (size_t)info.uncompressed_size;
        size_t name_len = strlen(entry.name);
        entry.is_directory = name_len > 0 && entry.name[name_len - 1] == '/';

        if (!callback(context, &entry)) break; /* caller asked to stop early - not a failure */
        go_result = unzGoToNextFile(uf);
    }
    if (result == BRUCE_OK && go_result != UNZ_END_OF_LIST_OF_FILE && go_result != UNZ_OK) result = BRUCE_ERR_IO;

    (void)unzClose(uf);
    return result;
}

/* ---- extract ---- */

/* Rejects an absolute entry name or one with a ".." path component - same
 * "zip-slip" guard as archive__tar_gz_extract()'s "tar-slip" one. */
static bool archive__zip_entry_name_is_safe(const char *name) {
    if (name[0] == '\0' || name[0] == '/') return false;
    for (const char *p = name; *p != '\0'; ++p) {
        bool at_component_start = p == name || p[-1] == '/';
        if (at_component_start && p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return false;
    }
    return true;
}

/* True if `name` (a raw stored entry name, e.g. "docs/readme.txt") is
 * `filter` itself or nested under it as a path component (e.g. filter
 * "docs" also matches "docs/readme.txt" and "docs/notes/todo.txt", but not
 * "docs.bak/readme.txt") - `filter` itself must have no trailing '/'
 * (archive__zip_extract_entry() strips one before calling in here). NULL
 * matches everything, for archive__zip_extract()'s whole-archive case. */
static bool archive__zip_name_matches_filter(const char *name, const char *filter) {
    if (filter == NULL) return true;
    size_t filter_len = strlen(filter);
    if (strncmp(name, filter, filter_len) != 0) return false;
    return name[filter_len] == '\0' || name[filter_len] == '/';
}

static bruce_result_t archive__zip_ensure_parent_dirs(const char *path) {
    char buffer[BRUCE_STORAGE_PATH_MAX];
    int written = snprintf(buffer, sizeof(buffer), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(buffer)) return BRUCE_ERR_RESOURCE_LIMIT;

    for (size_t i = 1; buffer[i] != '\0'; ++i) {
        if (buffer[i] != '/') continue;
        buffer[i] = '\0';
        bruce_result_t result = storage__mkdir(buffer);
        buffer[i] = '/';
        if (result != BRUCE_OK) return result;
    }
    return BRUCE_OK;
}

static bruce_result_t archive__zip_extract_current(unzFile uf, const char *dest_path) {
    if (unzOpenCurrentFile(uf) != UNZ_OK) return BRUCE_ERR_IO;

    bruce_file_id_t out_file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result =
        storage__open(dest_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &out_file);
    if (result == BRUCE_OK) {
        uint8_t chunk[ARCHIVE_ZIP_CHUNK_SIZE];
        for (;;) {
            int read_size = unzReadCurrentFile(uf, chunk, sizeof(chunk));
            if (read_size < 0) {
                result = BRUCE_ERR_IO;
                break;
            }
            if (read_size == 0) break;
            size_t written_size = 0;
            result = storage__write(out_file, chunk, (size_t)read_size, &written_size);
            if (result == BRUCE_OK && written_size != (size_t)read_size) result = BRUCE_ERR_IO;
            if (result != BRUCE_OK) break;
        }
        (void)storage__close(out_file);
    }
    (void)unzCloseCurrentFile(uf);
    return result;
}

/* `filter` == NULL extracts every entry (archive__zip_extract()'s own
 * shape); otherwise only entries matching it via
 * archive__zip_name_matches_filter() below are extracted, and
 * *out_matched_any reports whether anything did (archive__zip_extract_entry()
 * turns "filter given but nothing matched" into BRUCE_ERR_NOT_FOUND - a
 * plain whole-archive extract has no such notion of "not found"). */
static bruce_result_t
archive__zip_extract_filtered(const char *archive_path, const char *dest_dir, const char *filter, bool *out_matched_any) {
    if (archive_path == NULL || dest_dir == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (out_matched_any != NULL) *out_matched_any = false;

    zlib_filefunc64_def filefunc;
    archive__zip_filefunc(&filefunc);
    unzFile uf = unzOpen2_64(archive_path, &filefunc);
    if (uf == NULL) return BRUCE_ERR_NOT_FOUND;

    bruce_result_t result = BRUCE_OK;
    int go_result = unzGoToFirstFile(uf);
    while (go_result == UNZ_OK && result == BRUCE_OK) {
        unz_file_info64 info;
        char name[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
        if (unzGetCurrentFileInfo64(uf, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK) {
            result = BRUCE_ERR_IO;
            break;
        }
        if (!archive__zip_entry_name_is_safe(name)) {
            result = BRUCE_ERR_INVALID_PATH;
            break;
        }

        if (archive__zip_name_matches_filter(name, filter)) {
            if (out_matched_any != NULL) *out_matched_any = true;

            size_t name_len = strlen(name);
            bool is_directory = name_len > 0 && name[name_len - 1] == '/';
            size_t name_len_trimmed = is_directory ? name_len - 1 : name_len;

            char dest_path[BRUCE_STORAGE_PATH_MAX];
            int written = snprintf(dest_path, sizeof(dest_path), "%s/%.*s", dest_dir, (int)name_len_trimmed, name);
            if (written < 0 || (size_t)written >= sizeof(dest_path)) {
                result = BRUCE_ERR_RESOURCE_LIMIT;
                break;
            }

            if (is_directory) {
                result = storage__mkdir(dest_path);
            } else {
                result = archive__zip_ensure_parent_dirs(dest_path);
                if (result == BRUCE_OK) result = archive__zip_extract_current(uf, dest_path);
            }
            if (result != BRUCE_OK) break;
        }
        go_result = unzGoToNextFile(uf);
    }
    if (result == BRUCE_OK && go_result != UNZ_END_OF_LIST_OF_FILE && go_result != UNZ_OK) result = BRUCE_ERR_IO;

    (void)unzClose(uf);
    return result;
}

bruce_result_t archive__zip_extract(const char *archive_path, const char *dest_dir) {
    return archive__zip_extract_filtered(archive_path, dest_dir, NULL, NULL);
}

bruce_result_t archive__zip_extract_entry(const char *archive_path, const char *entry_name, const char *dest_dir) {
    /* A trailing '/' (a directory entry's own stored name, e.g. as browsed
     * from a bruce_archive_entry_t) would otherwise stop
     * archive__zip_name_matches_filter() from also matching anything nested
     * under it - see that function's comment. */
    char filter[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
    if (entry_name == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    size_t entry_len = strlen(entry_name);
    if (entry_len == 0 || entry_len >= sizeof(filter)) return BRUCE_ERR_INVALID_ARGUMENT;
    memcpy(filter, entry_name, entry_len + 1);
    if (filter[entry_len - 1] == '/') filter[entry_len - 1] = '\0';

    bool matched_any = false;
    bruce_result_t result = archive__zip_extract_filtered(archive_path, dest_dir, filter, &matched_any);
    if (result == BRUCE_OK && !matched_any) result = BRUCE_ERR_NOT_FOUND;
    return result;
}

/* ---- read entry ---- */

bruce_result_t archive__zip_read_entry(
    const char *archive_path, const char *entry_name, char *buffer, size_t buffer_size, size_t *out_size
) {
    if (archive_path == NULL || entry_name == NULL || entry_name[0] == '\0' || buffer == NULL || buffer_size == 0 ||
        out_size == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    zlib_filefunc64_def filefunc;
    archive__zip_filefunc(&filefunc);
    unzFile uf = unzOpen2_64(archive_path, &filefunc);
    if (uf == NULL) return BRUCE_ERR_NOT_FOUND;

    /* CASESENSITIVITY=0: minizip's own default, matching how
     * archive__zip_list()'s names are byte-for-byte what's stored - the
     * caller (archive_app's browser) always passes back a name that came
     * from exactly that listing. */
    bruce_result_t result = BRUCE_OK;
    if (unzLocateFile(uf, entry_name, 0) != UNZ_OK) {
        result = BRUCE_ERR_NOT_FOUND;
    } else {
        unz_file_info64 info;
        char name[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
        if (unzGetCurrentFileInfo64(uf, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK) {
            result = BRUCE_ERR_IO;
        } else {
            size_t name_len = strlen(name);
            if (name_len > 0 && name[name_len - 1] == '/') {
                result = BRUCE_ERR_INVALID_ARGUMENT; /* a directory, not a file */
            } else if (unzOpenCurrentFile(uf) != UNZ_OK) {
                result = BRUCE_ERR_IO;
            } else {
                size_t capacity = buffer_size - 1;
                size_t total_read = 0;
                while (total_read < capacity) {
                    unsigned chunk_size = (unsigned)(capacity - total_read);
                    int read_size = unzReadCurrentFile(uf, buffer + total_read, chunk_size);
                    if (read_size < 0) {
                        result = BRUCE_ERR_IO;
                        break;
                    }
                    if (read_size == 0) break;
                    total_read += (size_t)read_size;
                }
                buffer[total_read] = '\0';
                (void)unzCloseCurrentFile(uf);
                if (result == BRUCE_OK) *out_size = (size_t)info.uncompressed_size;
            }
        }
    }

    (void)unzClose(uf);
    return result;
}
