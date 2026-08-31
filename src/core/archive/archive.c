#include "core_sdk/archive.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <microtar.h>

#include "core_sdk/compress.h"
#include "core_sdk/storage.h"

/*
 * Two separate mtar_t backends, both plugged into microtar's own
 * read/write/seek/close function pointers (see microtar.h) instead of its
 * built-in stdio mtar_open():
 *
 *  - archive__gz_writer_* : archive__tar_gz_create()'s write path. Tar
 *    bytes handed to it are streamed straight through core_sdk/compress.h
 *    (gzip) into storage__write() - single pass, nothing buffered beyond
 *    one chunk. Never seeked (verified against microtar.c: writing only
 *    ever calls its own twrite(), never tar->seek()).
 *  - archive__storage_* : a plain storage__read/write/seek wrapper with no
 *    compression involved, used against the already-decompressed scratch
 *    ".tar" file archive__decompress_to_scratch() produces - this is what
 *    gives list/extract real seeking, which gzip decompression itself
 *    can't (see core_sdk/archive.h's module doc comment).
 */

#define ARCHIVE_CHUNK_SIZE 512

/* ---- gzip-compressing tar writer (archive__tar_gz_create) ---- */

typedef struct {
    bruce_file_id_t file;
    bruce_compress_ctx_t *compress_ctx;
    bruce_result_t error; /* sticky: first failure seen, so a later mtar_* call can report it */
} archive__gz_writer_t;

/* Feeds `size` bytes of `data` (already-formatted tar bytes) through gzip
 * compression and out to storage, looping until every pending output byte
 * has been drained; `finish` flushes and finalizes the gzip stream once
 * there's no more tar data coming (data/size are ignored then). */
static bool archive__gz_writer_feed(archive__gz_writer_t *writer, const void *data, size_t size, bool finish) {
    uint8_t out_buf[ARCHIVE_CHUNK_SIZE];
    bool fed = false;
    bool finished = false;
    for (;;) {
        size_t written = 0;
        bruce_result_t result = compress__update(
            writer->compress_ctx, fed ? NULL : data, fed ? 0 : size, finish, out_buf, sizeof(out_buf), &written,
            &finished
        );
        fed = true;
        if (result != BRUCE_OK) {
            writer->error = result;
            return false;
        }
        if (written > 0) {
            size_t stored = 0;
            result = storage__write(writer->file, out_buf, written, &stored);
            if (result != BRUCE_OK || stored != written) {
                writer->error = result != BRUCE_OK ? result : BRUCE_ERR_IO;
                return false;
            }
        }
        if (finished) return true;
        if (!finish && written < sizeof(out_buf)) return true; /* nothing more pending this round */
    }
}

static int archive__gz_writer_write_cb(mtar_t *tar, const void *data, unsigned size) {
    archive__gz_writer_t *writer = (archive__gz_writer_t *)tar->stream;
    return archive__gz_writer_feed(writer, data, size, false) ? MTAR_ESUCCESS : MTAR_EWRITEFAIL;
}

static int archive__gz_writer_read_cb(mtar_t *tar, void *data, unsigned size) {
    (void)tar;
    (void)data;
    (void)size;
    return MTAR_EFAILURE; /* write-only backend */
}

static int archive__gz_writer_seek_cb(mtar_t *tar, unsigned pos) {
    (void)tar;
    (void)pos;
    return MTAR_EFAILURE; /* never called for a straight write_header/write_data/finalize sequence */
}

static int archive__gz_writer_close_cb(mtar_t *tar) {
    (void)tar;
    return MTAR_ESUCCESS; /* archive__tar_gz_create() owns the storage/compress lifetimes directly */
}

static const char *archive__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bruce_result_t
archive__add_path(mtar_t *tar, archive__gz_writer_t *writer, const char *fs_path, const char *archive_name);

static bruce_result_t
archive__add_file(mtar_t *tar, archive__gz_writer_t *writer, const char *fs_path, const char *archive_name) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(fs_path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);
    if (result == BRUCE_OK && size > UINT32_MAX) result = BRUCE_ERR_RESOURCE_LIMIT; /* tar's size field is 32-bit */
    if (result != BRUCE_OK) {
        (void)storage__close(file);
        return result;
    }

    if (mtar_write_file_header(tar, archive_name, (unsigned)size) != MTAR_ESUCCESS) {
        (void)storage__close(file);
        return writer->error != BRUCE_OK ? writer->error : BRUCE_ERR_IO;
    }

    uint8_t chunk[ARCHIVE_CHUNK_SIZE];
    uint64_t remaining = size;
    while (remaining > 0) {
        size_t to_read = remaining < sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        size_t read_size = 0;
        result = storage__read(file, chunk, to_read, &read_size);
        if (result == BRUCE_OK && read_size != to_read) result = BRUCE_ERR_IO;
        if (result != BRUCE_OK) break;
        if (mtar_write_data(tar, chunk, (unsigned)read_size) != MTAR_ESUCCESS) {
            result = writer->error != BRUCE_OK ? writer->error : BRUCE_ERR_IO;
            break;
        }
        remaining -= read_size;
    }
    (void)storage__close(file);
    return result;
}

static bruce_result_t
archive__add_directory(mtar_t *tar, archive__gz_writer_t *writer, const char *fs_path, const char *archive_name) {
    char dir_name[BRUCE_ARCHIVE_ENTRY_NAME_MAX];
    int name_written = snprintf(dir_name, sizeof(dir_name), "%s/", archive_name);
    if (name_written < 0 || (size_t)name_written >= sizeof(dir_name)) return BRUCE_ERR_RESOURCE_LIMIT;
    if (mtar_write_dir_header(tar, dir_name) != MTAR_ESUCCESS) {
        return writer->error != BRUCE_OK ? writer->error : BRUCE_ERR_IO;
    }

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
        int name_written2 = snprintf(child_archive_name, sizeof(child_archive_name), "%s/%s", archive_name, entries[i].name);
        if (fs_written < 0 || (size_t)fs_written >= sizeof(child_fs_path) || name_written2 < 0 ||
            (size_t)name_written2 >= sizeof(child_archive_name)) {
            result = BRUCE_ERR_RESOURCE_LIMIT;
            break;
        }
        result = archive__add_path(tar, writer, child_fs_path, child_archive_name);
    }
    free(entries);
    return result;
}

static bruce_result_t
archive__add_path(mtar_t *tar, archive__gz_writer_t *writer, const char *fs_path, const char *archive_name) {
    size_t probe_count = 0;
    bool is_directory = storage__list(fs_path, NULL, 0, &probe_count) == BRUCE_OK;
    return is_directory ? archive__add_directory(tar, writer, fs_path, archive_name)
                         : archive__add_file(tar, writer, fs_path, archive_name);
}

bruce_result_t
archive__tar_gz_create(const char *archive_path, const char *const *entry_paths, size_t entry_count, int level) {
    if (archive_path == NULL || (entry_paths == NULL && entry_count > 0)) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result =
        storage__open(archive_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file);
    if (result != BRUCE_OK) return result;

    archive__gz_writer_t writer = {.file = file, .compress_ctx = NULL, .error = BRUCE_OK};
    writer.compress_ctx = compress__start(BRUCE_COMPRESS_FORMAT_GZIP, level);
    if (writer.compress_ctx == NULL) {
        (void)storage__close(file);
        return BRUCE_ERR_NO_MEMORY;
    }

    mtar_t tar;
    memset(&tar, 0, sizeof(tar));
    tar.write = archive__gz_writer_write_cb;
    tar.read = archive__gz_writer_read_cb;
    tar.seek = archive__gz_writer_seek_cb;
    tar.close = archive__gz_writer_close_cb;
    tar.stream = &writer;

    for (size_t i = 0; i < entry_count && result == BRUCE_OK; ++i) {
        result = archive__add_path(&tar, &writer, entry_paths[i], archive__basename(entry_paths[i]));
    }
    if (result == BRUCE_OK && mtar_finalize(&tar) != MTAR_ESUCCESS) {
        result = writer.error != BRUCE_OK ? writer.error : BRUCE_ERR_IO;
    }
    if (result == BRUCE_OK && !archive__gz_writer_feed(&writer, NULL, 0, true)) {
        result = writer.error != BRUCE_OK ? writer.error : BRUCE_ERR_IO;
    }

    compress__end(writer.compress_ctx);
    (void)storage__close(file);
    if (result != BRUCE_OK) (void)storage__remove(archive_path); /* don't leave a partial archive behind */
    return result;
}

/* ---- plain storage-backed tar reader (list/extract, over the scratch file) ---- */

typedef struct {
    bruce_file_id_t file;
} archive__storage_stream_t;

static int archive__storage_read_cb(mtar_t *tar, void *data, unsigned size) {
    archive__storage_stream_t *stream = (archive__storage_stream_t *)tar->stream;
    size_t read_size = 0;
    bruce_result_t result = storage__read(stream->file, data, size, &read_size);
    return (result == BRUCE_OK && read_size == size) ? MTAR_ESUCCESS : MTAR_EREADFAIL;
}

static int archive__storage_write_cb(mtar_t *tar, const void *data, unsigned size) {
    archive__storage_stream_t *stream = (archive__storage_stream_t *)tar->stream;
    size_t written = 0;
    bruce_result_t result = storage__write(stream->file, data, size, &written);
    return (result == BRUCE_OK && written == size) ? MTAR_ESUCCESS : MTAR_EWRITEFAIL;
}

static int archive__storage_seek_cb(mtar_t *tar, unsigned pos) {
    archive__storage_stream_t *stream = (archive__storage_stream_t *)tar->stream;
    bruce_result_t result = storage__seek(stream->file, (int64_t)pos, SEEK_SET, NULL);
    return result == BRUCE_OK ? MTAR_ESUCCESS : MTAR_ESEEKFAIL;
}

static int archive__storage_close_cb(mtar_t *tar) {
    (void)tar; /* caller owns the storage__open()/close() lifetime directly */
    return MTAR_ESUCCESS;
}

/* Fully decompresses `archive_path` to a same-directory "<archive_path>.tar_scratch"
 * file so the tar reader below can seek freely - see core_sdk/archive.h's
 * module doc comment for why this can't just decompress-on-demand instead. */
static bruce_result_t
archive__decompress_to_scratch(const char *archive_path, char *scratch_path, size_t scratch_path_size) {
    int path_written = snprintf(scratch_path, scratch_path_size, "%s.tar_scratch", archive_path);
    if (path_written < 0 || (size_t)path_written >= scratch_path_size) return BRUCE_ERR_RESOURCE_LIMIT;

    bruce_file_id_t in_file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(archive_path, BRUCE_STORAGE_OPEN_READ, &in_file);
    if (result != BRUCE_OK) return result;

    bruce_file_id_t out_file = BRUCE_FILE_ID_INVALID;
    result = storage__open(
        scratch_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &out_file
    );
    if (result != BRUCE_OK) {
        (void)storage__close(in_file);
        return result;
    }

    bruce_compress_ctx_t *ctx = decompress__start(BRUCE_COMPRESS_FORMAT_GZIP);
    if (ctx == NULL) {
        (void)storage__close(in_file);
        (void)storage__close(out_file);
        (void)storage__remove(scratch_path);
        return BRUCE_ERR_NO_MEMORY;
    }

    uint8_t in_buf[ARCHIVE_CHUNK_SIZE];
    uint8_t out_buf[ARCHIVE_CHUNK_SIZE];
    bool finished = false;
    while (result == BRUCE_OK && !finished) {
        size_t read_size = 0;
        result = storage__read(in_file, in_buf, sizeof(in_buf), &read_size);
        if (result != BRUCE_OK) break;
        if (read_size == 0) {
            result = BRUCE_ERR_IO; /* archive ended before the gzip stream ever reported finished - truncated */
            break;
        }

        bool fed = false;
        for (;;) {
            size_t written_out = 0;
            result = decompress__update(
                ctx, fed ? NULL : in_buf, fed ? 0 : read_size, out_buf, sizeof(out_buf), &written_out, &finished
            );
            fed = true;
            if (result != BRUCE_OK) break;
            if (written_out > 0) {
                size_t stored = 0;
                result = storage__write(out_file, out_buf, written_out, &stored);
                if (result == BRUCE_OK && stored != written_out) result = BRUCE_ERR_IO;
                if (result != BRUCE_OK) break;
            }
            if (finished || written_out < sizeof(out_buf)) break;
        }
    }

    decompress__end(ctx);
    (void)storage__close(in_file);
    (void)storage__close(out_file);
    if (result != BRUCE_OK) (void)storage__remove(scratch_path);
    return result;
}

typedef bruce_result_t (*archive__entry_visitor_fn)(mtar_t *tar, const mtar_header_t *header, void *context);

static bruce_result_t
archive__walk_scratch(const char *scratch_path, archive__entry_visitor_fn visitor, void *context) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(scratch_path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    archive__storage_stream_t stream = {.file = file};
    mtar_t tar;
    memset(&tar, 0, sizeof(tar));
    tar.read = archive__storage_read_cb;
    tar.write = archive__storage_write_cb;
    tar.seek = archive__storage_seek_cb;
    tar.close = archive__storage_close_cb;
    tar.stream = &stream;

    mtar_header_t header;
    int mtar_result;
    while ((mtar_result = mtar_read_header(&tar, &header)) == MTAR_ESUCCESS) {
        result = visitor(&tar, &header, context);
        if (result != BRUCE_OK) break;
        if (mtar_next(&tar) != MTAR_ESUCCESS) break;
    }
    (void)storage__close(file);

    /* MTAR_ENULLRECORD just means the trailing null blocks were reached -
     * the normal, successful end of the archive, not an error. */
    if (result == BRUCE_OK && mtar_result != MTAR_ENULLRECORD && mtar_result != MTAR_ESUCCESS) result = BRUCE_ERR_IO;
    return result;
}

/* ---- list ---- */

typedef struct {
    bruce_archive_list_fn callback;
    void *user_context;
} archive__list_visitor_ctx_t;

static bruce_result_t archive__list_visitor(mtar_t *tar, const mtar_header_t *header, void *context) {
    (void)tar;
    archive__list_visitor_ctx_t *ctx = (archive__list_visitor_ctx_t *)context;
    bruce_archive_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.name, header->name, sizeof(entry.name) - 1);
    entry.size = header->size;
    entry.is_directory = header->type == MTAR_TDIR;
    return ctx->callback(ctx->user_context, &entry) ? BRUCE_OK : BRUCE_ERR_CANCELLED;
}

bruce_result_t archive__tar_gz_list(const char *archive_path, bruce_archive_list_fn callback, void *context) {
    if (archive_path == NULL || callback == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    char scratch_path[BRUCE_STORAGE_PATH_MAX];
    bruce_result_t result = archive__decompress_to_scratch(archive_path, scratch_path, sizeof(scratch_path));
    if (result != BRUCE_OK) return result;

    archive__list_visitor_ctx_t visitor_ctx = {.callback = callback, .user_context = context};
    result = archive__walk_scratch(scratch_path, archive__list_visitor, &visitor_ctx);
    (void)storage__remove(scratch_path);
    return result == BRUCE_ERR_CANCELLED ? BRUCE_OK : result; /* caller asked to stop early - not a failure */
}

/* ---- extract ---- */

typedef struct {
    const char *dest_dir;
} archive__extract_visitor_ctx_t;

/* Rejects an absolute entry name or one with a ".." path component -
 * without this, a crafted (or just buggy) archive could write outside
 * dest_dir ("tar-slip"). */
static bool archive__entry_name_is_safe(const char *name) {
    if (name[0] == '\0' || name[0] == '/') return false;
    for (const char *p = name; *p != '\0'; ++p) {
        bool at_component_start = p == name || p[-1] == '/';
        if (at_component_start && p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return false;
    }
    return true;
}

static bruce_result_t archive__ensure_parent_dirs(const char *path) {
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

static bruce_result_t archive__extract_visitor(mtar_t *tar, const mtar_header_t *header, void *context) {
    archive__extract_visitor_ctx_t *ctx = (archive__extract_visitor_ctx_t *)context;
    if (!archive__entry_name_is_safe(header->name)) return BRUCE_ERR_INVALID_PATH;

    /* archive__add_directory() always writes a directory entry's name with
     * a trailing '/' - strip it so dest_path doesn't end up with one too. */
    size_t name_len = strlen(header->name);
    bool has_trailing_slash = name_len > 0 && header->name[name_len - 1] == '/';
    size_t name_len_trimmed = has_trailing_slash ? name_len - 1 : name_len;

    char dest_path[BRUCE_STORAGE_PATH_MAX];
    int written =
        snprintf(dest_path, sizeof(dest_path), "%s/%.*s", ctx->dest_dir, (int)name_len_trimmed, header->name);
    if (written < 0 || (size_t)written >= sizeof(dest_path)) return BRUCE_ERR_RESOURCE_LIMIT;

    if (header->type == MTAR_TDIR) return storage__mkdir(dest_path);

    bruce_result_t result = archive__ensure_parent_dirs(dest_path);
    if (result != BRUCE_OK) return result;

    bruce_file_id_t out_file = BRUCE_FILE_ID_INVALID;
    result =
        storage__open(dest_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &out_file);
    if (result != BRUCE_OK) return result;

    uint8_t chunk[ARCHIVE_CHUNK_SIZE];
    unsigned remaining = header->size;
    while (remaining > 0 && result == BRUCE_OK) {
        unsigned to_read = remaining < sizeof(chunk) ? remaining : (unsigned)sizeof(chunk);
        if (mtar_read_data(tar, chunk, to_read) != MTAR_ESUCCESS) {
            result = BRUCE_ERR_IO;
            break;
        }
        size_t written_size = 0;
        result = storage__write(out_file, chunk, to_read, &written_size);
        if (result == BRUCE_OK && written_size != to_read) result = BRUCE_ERR_IO;
        remaining -= to_read;
    }
    (void)storage__close(out_file);
    return result;
}

bruce_result_t archive__tar_gz_extract(const char *archive_path, const char *dest_dir) {
    if (archive_path == NULL || dest_dir == NULL) return BRUCE_ERR_INVALID_ARGUMENT;

    char scratch_path[BRUCE_STORAGE_PATH_MAX];
    bruce_result_t result = archive__decompress_to_scratch(archive_path, scratch_path, sizeof(scratch_path));
    if (result != BRUCE_OK) return result;

    archive__extract_visitor_ctx_t visitor_ctx = {.dest_dir = dest_dir};
    result = archive__walk_scratch(scratch_path, archive__extract_visitor, &visitor_ctx);
    (void)storage__remove(scratch_path);
    return result;
}
