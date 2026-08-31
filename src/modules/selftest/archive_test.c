#include "archive_test.h"

#include "core_sdk/archive.h"
#include "core_sdk/compress.h"
#include "core_sdk/storage.h"

#include <microtar.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * core_sdk/archive.h coverage: a create/list/extract roundtrip for each
 * format (tar.gz via microtar+gzip, zip via minizip), a corrupt-archive
 * case, and a tar-slip rejection case. The zip-slip guard shares the exact
 * same archive__*_entry_name_is_safe()-shaped check as tar's (see
 * core/archive/archive_zip.c), so one crafted-archive test covers the
 * logic both formats rely on rather than duplicating it with a
 * hand-assembled ".zip" too.
 */

static bruce_result_t archive_test__write_bytes(const char *path, const void *data, size_t size) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) return result;
    size_t written = 0;
    if (size > 0) result = storage__write(file, data, size, &written);
    if (result == BRUCE_OK && written != size) result = BRUCE_ERR_IO;
    (void)storage__close(file);
    return result;
}

static bruce_result_t archive_test__write_file(const char *path, const char *content) {
    return archive_test__write_bytes(path, content, strlen(content));
}

static bool archive_test__file_matches(const char *path, const char *expected) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return false;
    char buffer[256];
    size_t read_size = 0;
    bruce_result_t result = storage__read(file, buffer, sizeof(buffer) - 1, &read_size);
    (void)storage__close(file);
    if (result != BRUCE_OK) return false;
    buffer[read_size] = '\0';
    return strcmp(buffer, expected) == 0;
}

/* ------------------------------------------------------------------------ */
/* create/list/extract roundtrip (shared shape, one instance per format)    */
/* ------------------------------------------------------------------------ */

typedef struct {
    bool found_file;
    bool found_dir;
    bool found_nested;
    size_t file_size;
} archive_test__listing_t;

static bool archive_test__collect_entry(void *context, const bruce_archive_entry_t *entry) {
    archive_test__listing_t *listing = (archive_test__listing_t *)context;
    if (!entry->is_directory && strcmp(entry->name, "src.txt") == 0) {
        listing->found_file = true;
        listing->file_size = entry->size;
    } else if (entry->is_directory && strcmp(entry->name, "srcdir/") == 0) {
        listing->found_dir = true;
    } else if (!entry->is_directory && strcmp(entry->name, "srcdir/nested.txt") == 0) {
        listing->found_nested = true;
    }
    return true;
}

static const char ARCHIVE_TEST_FILE_CONTENT[] = "hello archive world\n";
static const char ARCHIVE_TEST_NESTED_CONTENT[] = "nested file\n";

/* `prefix` namespaces every path this uses (e.g. "/selftest_archive_tar")
 * so the tar.gz and zip instances of this test never touch each other's
 * files, however they're ordered/parallelized. `archive_path` is the
 * caller's already-namespaced "<prefix>.tar.gz"/"<prefix>.zip". */
static bool archive_test__roundtrip(const char *prefix, const char *archive_path, bool zip) {
    char src_file[96], src_dir[96], src_nested[96], dest_dir[96], dest_file[96], dest_nested[96];
    snprintf(src_file, sizeof(src_file), "%s_src.txt", prefix);
    snprintf(src_dir, sizeof(src_dir), "%s_srcdir", prefix);
    snprintf(src_nested, sizeof(src_nested), "%s_srcdir/nested.txt", prefix);
    snprintf(dest_dir, sizeof(dest_dir), "%s_dest", prefix);
    snprintf(dest_file, sizeof(dest_file), "%s_dest/src.txt", prefix);
    snprintf(dest_nested, sizeof(dest_nested), "%s_dest/srcdir/nested.txt", prefix);

    bruce_result_t result = archive_test__write_file(src_file, ARCHIVE_TEST_FILE_CONTENT);
    if (result == BRUCE_OK) result = storage__mkdir(src_dir);
    if (result == BRUCE_OK) result = archive_test__write_file(src_nested, ARCHIVE_TEST_NESTED_CONTENT);
    if (result == BRUCE_OK) result = storage__mkdir(dest_dir);

    const char *entry_paths[] = {src_file, src_dir};
    if (result == BRUCE_OK) {
        result = zip ? archive__zip_create(archive_path, entry_paths, 2, BRUCE_COMPRESS_LEVEL_DEFAULT)
                      : archive__tar_gz_create(archive_path, entry_paths, 2, BRUCE_COMPRESS_LEVEL_DEFAULT);
    }

    archive_test__listing_t listing = {0};
    if (result == BRUCE_OK) {
        result = zip ? archive__zip_list(archive_path, archive_test__collect_entry, &listing)
                      : archive__tar_gz_list(archive_path, archive_test__collect_entry, &listing);
    }

    if (result == BRUCE_OK) {
        result = zip ? archive__zip_extract(archive_path, dest_dir) : archive__tar_gz_extract(archive_path, dest_dir);
    }

    bool listing_ok = listing.found_file && listing.found_dir && listing.found_nested &&
                       listing.file_size == strlen(ARCHIVE_TEST_FILE_CONTENT);
    bool extract_ok = result == BRUCE_OK && archive_test__file_matches(dest_file, ARCHIVE_TEST_FILE_CONTENT) &&
                       archive_test__file_matches(dest_nested, ARCHIVE_TEST_NESTED_CONTENT);
    bool ok = result == BRUCE_OK && listing_ok && extract_ok;

    printf(
        "[selftest] archive/%s_roundtrip: %s (result=%d listing_ok=%d extract_ok=%d)\n", zip ? "zip" : "tar_gz",
        ok ? "OK" : "FAIL", result, listing_ok, extract_ok
    );

    (void)storage__remove(dest_nested);
    (void)storage__remove(dest_file);
    snprintf(dest_nested, sizeof(dest_nested), "%s_dest/srcdir", prefix);
    (void)storage__remove(dest_nested);
    (void)storage__remove(dest_dir);
    (void)storage__remove(archive_path);
    (void)storage__remove(src_nested);
    (void)storage__remove(src_dir);
    (void)storage__remove(src_file);
    return ok;
}

bool selftest__run_archive_tar_gz_roundtrip_case(void) {
    return archive_test__roundtrip("/selftest_archive_tar", "/selftest_archive_tar.tar.gz", false);
}

bool selftest__run_archive_zip_roundtrip_case(void) {
    return archive_test__roundtrip("/selftest_archive_zip", "/selftest_archive_zip.zip", true);
}

/* ------------------------------------------------------------------------ */
/* corrupt archive                                                          */
/* ------------------------------------------------------------------------ */

bool selftest__run_archive_tar_gz_corrupt_case(void) {
    static const char path[] = "/selftest_archive_corrupt.tar.gz";
    /* Not a valid gzip stream (no 0x1f 0x8b magic) - archive__tar_gz_list()
     * must report a clean error rather than crash or claim success. */
    static const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};

    bruce_result_t write_result = archive_test__write_bytes(path, garbage, sizeof(garbage));
    bruce_result_t list_result = archive__tar_gz_list(path, archive_test__collect_entry, NULL);
    bool ok = write_result == BRUCE_OK && list_result != BRUCE_OK;

    printf("[selftest] archive/tar_gz_corrupt: %s (list_result=%d)\n", ok ? "OK" : "FAIL", list_result);
    (void)storage__remove(path);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* tar-slip rejection                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    bruce_file_id_t file;
} archive_test__stream_t;

static int archive_test__write_cb(mtar_t *tar, const void *data, unsigned size) {
    archive_test__stream_t *stream = (archive_test__stream_t *)tar->stream;
    size_t written = 0;
    bruce_result_t result = storage__write(stream->file, data, size, &written);
    return (result == BRUCE_OK && written == size) ? MTAR_ESUCCESS : MTAR_EWRITEFAIL;
}

static int archive_test__read_cb(mtar_t *tar, void *data, unsigned size) {
    (void)tar;
    (void)data;
    (void)size;
    return MTAR_EFAILURE; /* write-only backend, same as archive.c's own gz writer */
}

static int archive_test__seek_cb(mtar_t *tar, unsigned pos) {
    (void)tar;
    (void)pos;
    return MTAR_EFAILURE; /* never called while only writing (verified against microtar.c) */
}

static int archive_test__close_cb(mtar_t *tar) {
    (void)tar; /* caller owns the storage__open()/close() lifetime directly */
    return MTAR_ESUCCESS;
}

/* Hand-assembles a tar (via microtar's own public write API, so the header
 * bytes themselves are exactly what a real tar would produce) whose single
 * entry's name is a path-traversal attempt, gzips it, and confirms
 * archive__tar_gz_extract() refuses it - and, just as important, never
 * actually creates the file outside dest_dir. */
bool selftest__run_archive_tar_gz_slip_rejection_case(void) {
    static const char scratch_path[] = "/selftest_archive_slip.tar";
    static const char archive_path[] = "/selftest_archive_slip.tar.gz";
    static const char dest_dir[] = "/selftest_archive_slip_dest";
    static const char evil_name[] = "../selftest_archive_slip_evil.txt";
    static const char evil_content[] = "pwned";
    static const char escaped_path[] = "/selftest_archive_slip_evil.txt"; /* dest_dir's parent, i.e. "/" */

    bruce_file_id_t scratch_file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        scratch_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &scratch_file
    );

    archive_test__stream_t stream = {.file = scratch_file};
    mtar_t tar = {0};
    tar.write = archive_test__write_cb;
    tar.read = archive_test__read_cb;
    tar.seek = archive_test__seek_cb;
    tar.close = archive_test__close_cb;
    tar.stream = &stream;

    if (result == BRUCE_OK) {
        result = mtar_write_file_header(&tar, evil_name, (unsigned)strlen(evil_content)) == MTAR_ESUCCESS
                     ? BRUCE_OK
                     : BRUCE_ERR_IO;
    }
    if (result == BRUCE_OK) {
        result = mtar_write_data(&tar, evil_content, (unsigned)strlen(evil_content)) == MTAR_ESUCCESS ? BRUCE_OK
                                                                                                        : BRUCE_ERR_IO;
    }
    if (result == BRUCE_OK) result = mtar_finalize(&tar) == MTAR_ESUCCESS ? BRUCE_OK : BRUCE_ERR_IO;
    if (scratch_file != BRUCE_FILE_ID_INVALID) (void)storage__close(scratch_file);

    /* Gzip the scratch tar into the real ".tar.gz" test archive - small
     * enough for archive.h's own one-shot API. */
    char tar_bytes[2048];
    size_t tar_size = 0;
    if (result == BRUCE_OK) {
        bruce_file_id_t read_file = BRUCE_FILE_ID_INVALID;
        result = storage__open(scratch_path, BRUCE_STORAGE_OPEN_READ, &read_file);
        if (result == BRUCE_OK) {
            result = storage__read(read_file, tar_bytes, sizeof(tar_bytes), &tar_size);
            (void)storage__close(read_file);
        }
    }
    char gz_bytes[2048];
    size_t gz_size = 0;
    if (result == BRUCE_OK) {
        result = compress__compute(
            BRUCE_COMPRESS_FORMAT_GZIP, BRUCE_COMPRESS_LEVEL_DEFAULT, tar_bytes, tar_size, gz_bytes, sizeof(gz_bytes),
            &gz_size
        );
    }
    if (result == BRUCE_OK) result = archive_test__write_bytes(archive_path, gz_bytes, gz_size);
    if (result == BRUCE_OK) result = storage__mkdir(dest_dir);

    bruce_result_t extract_result = result == BRUCE_OK ? archive__tar_gz_extract(archive_path, dest_dir) : result;
    bool escaped_exists = false;
    (void)storage__exists(escaped_path, &escaped_exists);
    bool ok = result == BRUCE_OK && extract_result != BRUCE_OK && !escaped_exists;

    printf(
        "[selftest] archive/tar_gz_slip_rejection: %s (setup_result=%d extract_result=%d escaped_exists=%d)\n",
        ok ? "OK" : "FAIL", result, extract_result, escaped_exists
    );

    (void)storage__remove(escaped_path); /* in case the guard failed - don't leave it behind either way */
    (void)storage__remove(dest_dir);
    (void)storage__remove(archive_path);
    (void)storage__remove(scratch_path);
    return ok;
}
