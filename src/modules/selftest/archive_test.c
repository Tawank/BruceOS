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

/* Same rule archive__basename() (archive.c, static) applies when *_create()
 * names an entry after the path it was given - duplicated here so the test
 * can predict the name without reaching into archive.c's internals. */
static const char *archive_test__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
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
    /* Archive entries are named by archive__basename() of the paths handed
     * to *_create() - since the roundtrip test prefixes every path (see
     * archive_test__roundtrip()'s own doc comment) those basenames are
     * "<prefix>_src.txt"/"<prefix>_srcdir/"/"<prefix>_srcdir/nested.txt",
     * not the literal "src.txt" the un-namespaced paths would produce, so
     * the expected names have to be computed per-call rather than hardcoded. */
    const char *expected_file_name;
    const char *expected_dir_name; /* includes the trailing '/' */
    const char *expected_nested_name;
    bool found_file;
    bool found_dir;
    bool found_nested;
    size_t file_size;
} archive_test__listing_t;

static bool archive_test__collect_entry(void *context, const bruce_archive_entry_t *entry) {
    archive_test__listing_t *listing = (archive_test__listing_t *)context;
    if (!entry->is_directory && strcmp(entry->name, listing->expected_file_name) == 0) {
        listing->found_file = true;
        listing->file_size = entry->size;
    } else if (entry->is_directory && strcmp(entry->name, listing->expected_dir_name) == 0) {
        listing->found_dir = true;
    } else if (!entry->is_directory && strcmp(entry->name, listing->expected_nested_name) == 0) {
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
    char src_file[128], src_dir[128], src_nested[128], dest_dir[128], dest_file[128], dest_nested[128];
    snprintf(src_file, sizeof(src_file), "%s_src.txt", prefix);
    snprintf(src_dir, sizeof(src_dir), "%s_srcdir", prefix);
    snprintf(src_nested, sizeof(src_nested), "%s_srcdir/nested.txt", prefix);
    snprintf(dest_dir, sizeof(dest_dir), "%s_dest", prefix);

    /* archive__basename() (and this test's own zip equivalent) strips
     * everything up to the last '/', so the entries actually end up named
     * after src_file/src_dir's basenames, not the literal "src.txt"/"srcdir" -
     * mirror that here instead of assuming the un-prefixed names, both for
     * what the listing callback should see and for where extract writes. */
    const char *file_basename = archive_test__basename(src_file);
    const char *dir_basename = archive_test__basename(src_dir);
    snprintf(dest_file, sizeof(dest_file), "%.60s/%.60s", dest_dir, file_basename);
    snprintf(dest_nested, sizeof(dest_nested), "%.50s/%.50s/nested.txt", dest_dir, dir_basename);

    bruce_result_t result = archive_test__write_file(src_file, ARCHIVE_TEST_FILE_CONTENT);
    if (result == BRUCE_OK) result = storage__mkdir(src_dir);
    if (result == BRUCE_OK) result = archive_test__write_file(src_nested, ARCHIVE_TEST_NESTED_CONTENT);
    if (result == BRUCE_OK) result = storage__mkdir(dest_dir);

    const char *entry_paths[] = {src_file, src_dir};
    if (result == BRUCE_OK) {
        result = zip ? archive__zip_create(archive_path, entry_paths, 2, BRUCE_COMPRESS_LEVEL_DEFAULT)
                      : archive__tar_gz_create(archive_path, entry_paths, 2, BRUCE_COMPRESS_LEVEL_DEFAULT);
    }

    /* dir_basename points into src_dir[128], but it's an opaque `const char *`
     * by the time it gets here, so GCC's format-truncation check can't see
     * that bound and assumes the worst case - the explicit ".100s" precision
     * gives it one to prove instead. */
    char expected_dir_name[128];
    snprintf(expected_dir_name, sizeof(expected_dir_name), "%.100s/", dir_basename);
    char expected_nested_name[128];
    snprintf(expected_nested_name, sizeof(expected_nested_name), "%.100s/nested.txt", dir_basename);
    archive_test__listing_t listing = {
        .expected_file_name = file_basename,
        .expected_dir_name = expected_dir_name,
        .expected_nested_name = expected_nested_name,
    };
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
    char dest_subdir[128];
    snprintf(dest_subdir, sizeof(dest_subdir), "%.60s/%.60s", dest_dir, dir_basename);
    (void)storage__remove(dest_subdir);
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

/* zip's corrupt-archive path is a completely separate code path from
 * tar.gz's (unzOpen2_64()/minizip vs. gzip+microtar - see this file's module
 * doc comment), so the case above doesn't exercise it: archive__zip_list()
 * must report a clean error for garbage bytes too, not crash or claim an
 * empty-but-successful listing. */
bool selftest__run_archive_zip_corrupt_case(void) {
    static const char path[] = "/selftest_archive_corrupt.zip";
    /* Not a valid zip (no "PK\x03\x04" local-file-header signature, and no
     * end-of-central-directory record for unzOpen2_64() to locate at all). */
    static const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};

    bruce_result_t write_result = archive_test__write_bytes(path, garbage, sizeof(garbage));
    bruce_result_t list_result = archive__zip_list(path, archive_test__collect_entry, NULL);
    bool ok = write_result == BRUCE_OK && list_result != BRUCE_OK;

    printf("[selftest] archive/zip_corrupt: %s (list_result=%d)\n", ok ? "OK" : "FAIL", list_result);
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

/* ------------------------------------------------------------------------ */
/* nonexistent archive path                                                 */
/* ------------------------------------------------------------------------ */

/* A missing source archive is a routine bad-path case (typo'd filename,
 * already-deleted download, etc.) - every list()/extract() entry point
 * across both formats must report a clean storage-style error instead of
 * crashing or dereferencing whatever storage__open() left behind. */
bool selftest__run_archive_not_found_case(void) {
    static const char missing_tar_gz[] = "/selftest_archive_missing.tar.gz";
    static const char missing_zip[] = "/selftest_archive_missing.zip";
    static const char dest_dir[] = "/selftest_archive_missing_dest";

    /* Belt-and-suspenders: clear out any previous run's leftovers first, so
     * this can't pass for the wrong reason (an old archive genuinely being
     * there rather than the not-found path actually being exercised). */
    (void)storage__remove(missing_tar_gz);
    (void)storage__remove(missing_zip);

    bruce_result_t tar_list_result = archive__tar_gz_list(missing_tar_gz, archive_test__collect_entry, NULL);
    bruce_result_t tar_extract_result = archive__tar_gz_extract(missing_tar_gz, dest_dir);
    bruce_result_t zip_list_result = archive__zip_list(missing_zip, archive_test__collect_entry, NULL);
    bruce_result_t zip_extract_result = archive__zip_extract(missing_zip, dest_dir);

    bool dest_created = false;
    (void)storage__exists(dest_dir, &dest_created);

    bool ok = tar_list_result != BRUCE_OK && tar_extract_result != BRUCE_OK && zip_list_result != BRUCE_OK &&
              zip_extract_result != BRUCE_OK && !dest_created;

    printf(
        "[selftest] archive/not_found: %s (tar_list=%d tar_extract=%d zip_list=%d zip_extract=%d dest_created=%d)\n",
        ok ? "OK" : "FAIL", tar_list_result, tar_extract_result, zip_list_result, zip_extract_result, dest_created
    );

    if (dest_created) (void)storage__remove(dest_dir);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* extract_entry / read_entry: single-file and single-subtree granularity   */
/* ------------------------------------------------------------------------ */

/* Builds the same "<prefix>_src.txt / <prefix>_srcdir/nested.txt" shape
 * archive_test__roundtrip() does, then exercises
 * archive__*_read_entry()/archive__*_extract_entry() - the API
 * archive_app.c's browser (modules/archive/archive_app.c) uses for its
 * per-item View/Extract, restricted to one entry or subtree rather than
 * archive_test__roundtrip()'s whole-archive archive__*_list()/_extract().
 * The point of each half below is proving the filter is actually SCOPED -
 * extracting just the top-level file must not also recreate the directory
 * subtree, and vice versa - not merely that these functions return
 * BRUCE_OK for something. */
static bool archive_test__entry_operations(const char *prefix, const char *archive_path, bool zip) {
    char src_file[128], src_dir[128], src_nested[128], dest_dir[128];
    snprintf(src_file, sizeof(src_file), "%s_src.txt", prefix);
    snprintf(src_dir, sizeof(src_dir), "%s_srcdir", prefix);
    snprintf(src_nested, sizeof(src_nested), "%s_srcdir/nested.txt", prefix);
    snprintf(dest_dir, sizeof(dest_dir), "%s_dest", prefix);

    const char *file_basename = archive_test__basename(src_file);
    const char *dir_basename = archive_test__basename(src_dir);

    bruce_result_t result = archive_test__write_file(src_file, ARCHIVE_TEST_FILE_CONTENT);
    if (result == BRUCE_OK) result = storage__mkdir(src_dir);
    if (result == BRUCE_OK) result = archive_test__write_file(src_nested, ARCHIVE_TEST_NESTED_CONTENT);
    if (result == BRUCE_OK) result = storage__mkdir(dest_dir);

    const char *entry_paths[] = {src_file, src_dir};
    if (result == BRUCE_OK) {
        result = zip ? archive__zip_create(archive_path, entry_paths, 2, BRUCE_COMPRESS_LEVEL_DEFAULT)
                      : archive__tar_gz_create(archive_path, entry_paths, 2, BRUCE_COMPRESS_LEVEL_DEFAULT);
    }

    /* read_entry: the top-level file's content comes back exactly, a
     * directory entry is rejected (not a file), and a name that doesn't
     * exist is BRUCE_ERR_NOT_FOUND. */
    char read_buffer[256];
    size_t read_size = 0;
    bruce_result_t read_result = BRUCE_ERR_INVALID_STATE;
    if (result == BRUCE_OK) {
        read_result = zip ? archive__zip_read_entry(archive_path, file_basename, read_buffer, sizeof(read_buffer), &read_size)
                           : archive__tar_gz_read_entry(
                                 archive_path, file_basename, read_buffer, sizeof(read_buffer), &read_size
                             );
    }
    bool read_ok = read_result == BRUCE_OK && read_size == strlen(ARCHIVE_TEST_FILE_CONTENT) &&
                   strcmp(read_buffer, ARCHIVE_TEST_FILE_CONTENT) == 0;

    char dir_entry_name[128];
    snprintf(dir_entry_name, sizeof(dir_entry_name), "%.100s/", dir_basename);
    bruce_result_t read_dir_result =
        zip ? archive__zip_read_entry(archive_path, dir_entry_name, read_buffer, sizeof(read_buffer), &read_size)
            : archive__tar_gz_read_entry(archive_path, dir_entry_name, read_buffer, sizeof(read_buffer), &read_size);
    bool read_dir_ok = read_dir_result == BRUCE_ERR_INVALID_ARGUMENT;

    bruce_result_t read_missing_result =
        zip ? archive__zip_read_entry(archive_path, "does_not_exist.txt", read_buffer, sizeof(read_buffer), &read_size)
            : archive__tar_gz_read_entry(
                  archive_path, "does_not_exist.txt", read_buffer, sizeof(read_buffer), &read_size
              );
    bool read_missing_ok = read_missing_result == BRUCE_ERR_NOT_FOUND;

    /* extract_entry: extracting just the top-level file must not also
     * recreate the directory subtree ... */
    char dest_file[128], dest_nested[128], dest_subdir[128];
    snprintf(dest_file, sizeof(dest_file), "%.60s/%.60s", dest_dir, file_basename);
    snprintf(dest_subdir, sizeof(dest_subdir), "%.60s/%.60s", dest_dir, dir_basename);
    snprintf(dest_nested, sizeof(dest_nested), "%.50s/%.50s/nested.txt", dest_dir, dir_basename);

    bruce_result_t extract_file_result = zip ? archive__zip_extract_entry(archive_path, file_basename, dest_dir)
                                              : archive__tar_gz_extract_entry(archive_path, file_basename, dest_dir);
    bool file_exists = false;
    bool subdir_created = true;
    (void)storage__exists(dest_file, &file_exists);
    (void)storage__exists(dest_subdir, &subdir_created);
    bool extract_file_ok = extract_file_result == BRUCE_OK && file_exists &&
                            archive_test__file_matches(dest_file, ARCHIVE_TEST_FILE_CONTENT) && !subdir_created;
    (void)storage__remove(dest_file);

    /* ... and, the other way round, extracting the directory must not
     * re-create the top-level file. */
    bruce_result_t extract_dir_result = zip ? archive__zip_extract_entry(archive_path, dir_basename, dest_dir)
                                             : archive__tar_gz_extract_entry(archive_path, dir_basename, dest_dir);
    bool nested_ok = archive_test__file_matches(dest_nested, ARCHIVE_TEST_NESTED_CONTENT);
    bool file_recreated = true;
    (void)storage__exists(dest_file, &file_recreated);
    bool extract_dir_ok = extract_dir_result == BRUCE_OK && nested_ok && !file_recreated;

    bruce_result_t extract_missing_result = zip ? archive__zip_extract_entry(archive_path, "does_not_exist.txt", dest_dir)
                                                 : archive__tar_gz_extract_entry(archive_path, "does_not_exist.txt", dest_dir);
    bool extract_missing_ok = extract_missing_result == BRUCE_ERR_NOT_FOUND;

    bool ok = result == BRUCE_OK && read_ok && read_dir_ok && read_missing_ok && extract_file_ok && extract_dir_ok &&
              extract_missing_ok;

    printf(
        "[selftest] archive/%s_entry_ops: %s (setup=%d read=%d read_dir=%d read_missing=%d extract_file=%d "
        "extract_dir=%d extract_missing=%d)\n",
        zip ? "zip" : "tar_gz", ok ? "OK" : "FAIL", result, read_ok, read_dir_ok, read_missing_ok, extract_file_ok,
        extract_dir_ok, extract_missing_ok
    );

    (void)storage__remove(dest_nested);
    (void)storage__remove(dest_subdir);
    (void)storage__remove(dest_dir);
    (void)storage__remove(archive_path);
    (void)storage__remove(src_nested);
    (void)storage__remove(src_dir);
    (void)storage__remove(src_file);
    return ok;
}

bool selftest__run_archive_tar_gz_entry_ops_case(void) {
    return archive_test__entry_operations(
        "/selftest_archive_tar_entry", "/selftest_archive_tar_entry.tar.gz", false
    );
}

bool selftest__run_archive_zip_entry_ops_case(void) {
    return archive_test__entry_operations("/selftest_archive_zip_entry", "/selftest_archive_zip_entry.zip", true);
}
