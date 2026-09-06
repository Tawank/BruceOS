#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/environment.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "modules/loaders/elf/elf_loader_sdk_symbols_test.h"
#include "modules/loaders/wasm/wasm_loader_app.h"
#include "platform_api_vmcore.h"

#include "elf_loader_test.h"

/*
 * Exercises the FILE*-based stdio adapters the ELF loader hands to
 * sandboxed apps (elf_loader_sdk_symbols.c, backed by storage__), by
 * calling the (deliberately non-static) adapter functions directly rather
 * than through a cross-compiled ELF fixture -- there is nothing
 * ELF-specific about the adapter logic itself, just mode-string parsing,
 * flag mapping, and eof/error tracking on top of core_sdk/storage.h.
 */
bool selftest__run_elf_loader_stdio_case(void) {
    const char *path = "/selftest_elf_stdio.txt";
    storage__remove(path);

    static const char text[] = "line one\nline two";

    FILE *write_stream = bruce_elf__fopen(path, "w");
    if (write_stream == NULL) {
        printf("[selftest] loader/elf_stdio: fopen(w) failed\n");
        return false;
    }
    size_t written = bruce_elf__fwrite(text, 1, sizeof(text) - 1, write_stream);
    int printf_result = bruce_elf__fprintf(write_stream, "%s", "!");
    if (written != sizeof(text) - 1 || printf_result != 1 || bruce_elf__fclose(write_stream) != 0) {
        printf("[selftest] loader/elf_stdio: write/fprintf/fclose failed\n");
        storage__remove(path);
        return false;
    }

    FILE *read_stream = bruce_elf__fopen(path, "r");
    if (read_stream == NULL) {
        printf("[selftest] loader/elf_stdio: fopen(r) failed\n");
        storage__remove(path);
        return false;
    }

    char first_line[32] = {0};
    if (bruce_elf__fgets(first_line, sizeof(first_line), read_stream) == NULL ||
        strcmp(first_line, "line one\n") != 0) {
        printf("[selftest] loader/elf_stdio: fgets mismatch (\"%s\")\n", first_line);
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }

    long after_first_line = bruce_elf__ftell(read_stream);
    if (after_first_line != (long)strlen("line one\n")) {
        printf("[selftest] loader/elf_stdio: ftell mismatch (%ld)\n", after_first_line);
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }

    char rest[32] = {0};
    size_t rest_read = bruce_elf__fread(rest, 1, sizeof(rest) - 1, read_stream);
    if (rest_read != strlen("line two!") || strcmp(rest, "line two!") != 0 ||
        !bruce_elf__feof(read_stream) || bruce_elf__ferror(read_stream)) {
        printf("[selftest] loader/elf_stdio: fread/feof mismatch (\"%s\", %zu bytes)\n", rest, rest_read);
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }

    bruce_elf__rewind(read_stream);
    if (bruce_elf__feof(read_stream) || bruce_elf__ftell(read_stream) != 0) {
        printf("[selftest] loader/elf_stdio: rewind did not reset position/eof\n");
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }
    bruce_elf__fclose(read_stream);
    storage__remove(path);

    bool missing_exists = true;
    storage__exists(path, &missing_exists);
    errno = 0;
    if (bruce_elf__fopen(path, "r") != NULL || errno != ENOENT || missing_exists) {
        printf("[selftest] loader/elf_stdio: missing-file fopen(r) did not fail with ENOENT\n");
        return false;
    }

    /* stdout/stdin are static console sentinels, not storage__-backed files
     * -- verify writes route through stdio__write (by return value; actual
     * console output isn't asserted on), the mismatched direction fails
     * cleanly instead of touching a storage handle that doesn't exist for
     * them, they're not seekable, and closing them is a harmless no-op
     * rather than freeing static storage. */
    static const char greeting[] = "selftest\n";
    if (bruce_elf__fwrite(greeting, 1, sizeof(greeting) - 1, bruce_elf__stdout_ptr) != sizeof(greeting) - 1) {
        printf("[selftest] loader/elf_stdio: fwrite(stdout) did not report full write\n");
        return false;
    }
    char sink[1];
    if (bruce_elf__fread(sink, 1, sizeof(sink), bruce_elf__stdout_ptr) != 0 ||
        !bruce_elf__ferror(bruce_elf__stdout_ptr)) {
        printf("[selftest] loader/elf_stdio: fread(stdout) did not fail\n");
        return false;
    }
    bruce_elf__clearerr(bruce_elf__stdout_ptr);
    if (bruce_elf__fwrite(sink, 1, sizeof(sink), bruce_elf__stdin_ptr) != 0 ||
        !bruce_elf__ferror(bruce_elf__stdin_ptr)) {
        printf("[selftest] loader/elf_stdio: fwrite(stdin) did not fail\n");
        return false;
    }
    bruce_elf__clearerr(bruce_elf__stdin_ptr);
    errno = 0;
    if (bruce_elf__fseek(bruce_elf__stdout_ptr, 0, SEEK_SET) != -1 || errno != ESPIPE) {
        printf("[selftest] loader/elf_stdio: fseek(stdout) did not fail with ESPIPE\n");
        return false;
    }
    if (bruce_elf__fclose(bruce_elf__stdout_ptr) != 0 || bruce_elf__fclose(bruce_elf__stdin_ptr) != 0) {
        printf("[selftest] loader/elf_stdio: fclose(stdout/stdin) was not a no-op\n");
        return false;
    }
    if (bruce_elf__fprintf(bruce_elf__stdout_ptr, "%s", "selftest\n") != (int)sizeof(greeting) - 1) {
        printf("[selftest] loader/elf_stdio: fprintf(stdout) after fclose failed\n");
        return false;
    }

    printf("[selftest] loader/elf_stdio: OK\n");
    return true;
}

/*
 * Exercises the getenv/setenv/unsetenv and strdup/strndup adapters
 * (elf_loader_sdk_symbols.c), by calling the (deliberately non-static)
 * adapter functions directly, same rationale as selftest__run_elf_loader_stdio_case().
 */
bool selftest__run_elf_loader_libc_case(void) {
    static const char name[] = "SELFTEST_ELF_LIBC_VAR";
    environment__unset(name);

    if (bruce_elf__getenv(name) != NULL) {
        printf("[selftest] loader/elf_libc: getenv saw a stale value\n");
        return false;
    }

    if (bruce_elf__setenv(name, "first", 0) != 0 || strcmp(environment__get(name), "first") != 0) {
        printf("[selftest] loader/elf_libc: setenv(overwrite=0) on a new var failed\n");
        return false;
    }
    if (bruce_elf__setenv(name, "second", 0) != 0 || strcmp(bruce_elf__getenv(name), "first") != 0) {
        printf("[selftest] loader/elf_libc: setenv(overwrite=0) clobbered an existing value\n");
        return false;
    }
    if (bruce_elf__setenv(name, "second", 1) != 0 || strcmp(bruce_elf__getenv(name), "second") != 0) {
        printf("[selftest] loader/elf_libc: setenv(overwrite=1) did not replace the value\n");
        return false;
    }
    if (bruce_elf__unsetenv(name) != 0 || bruce_elf__getenv(name) != NULL) {
        printf("[selftest] loader/elf_libc: unsetenv did not clear the value\n");
        return false;
    }

    static const char text[] = "bruce elf strdup";
    char *duplicate = bruce_elf__strdup(text);
    if (duplicate == NULL || duplicate == text || strcmp(duplicate, text) != 0) {
        printf("[selftest] loader/elf_libc: strdup mismatch\n");
        return false;
    }
    memory__free(duplicate);

    char *partial = bruce_elf__strndup(text, 5);
    if (partial == NULL || strcmp(partial, "bruce") != 0) {
        printf("[selftest] loader/elf_libc: strndup mismatch (\"%s\")\n", partial ? partial : "(null)");
        memory__free(partial);
        return false;
    }
    memory__free(partial);

    printf("[selftest] loader/elf_libc: OK\n");
    return true;
}

/*
 * Exercises the ELF loader's time.h support (elf_loader_sdk_symbols.c).
 * time()/gmtime_r() are exercised directly -- they're exported to sandboxed
 * apps unadapted, so this is really regression coverage for the real
 * picolibc functions against known reference dates, not just a
 * self-round-trip that could hide a wrong assumption. localtime_r()/
 * mktime()/clock() ARE BruceOS-specific adapters (deliberately non-static
 * so this case can call them directly, same rationale as
 * selftest__run_elf_loader_stdio_case()), so those are checked for
 * mktime(localtime_r(t)) == t and for actually applying Config's currently
 * configured offset rather than silently passing UTC through.
 */
bool selftest__run_elf_loader_time_case(void) {
    /* 1970-01-01 00:00:00 UTC, a Thursday -- the epoch itself. */
    time_t epoch0 = 0;
    struct tm tm0;
    if (gmtime_r(&epoch0, &tm0) == NULL) {
        printf("[selftest] loader/elf_time: gmtime_r(0) returned NULL\n");
        return false;
    }
    if (tm0.tm_year != 70 || tm0.tm_mon != 0 || tm0.tm_mday != 1 || tm0.tm_hour != 0 ||
        tm0.tm_min != 0 || tm0.tm_sec != 0 || tm0.tm_wday != 4 || tm0.tm_yday != 0) {
        printf("[selftest] loader/elf_time: gmtime_r(0) mismatch (y=%d m=%d d=%d h=%d mi=%d s=%d wd=%d yd=%d)\n",
               tm0.tm_year, tm0.tm_mon, tm0.tm_mday, tm0.tm_hour, tm0.tm_min, tm0.tm_sec, tm0.tm_wday,
               tm0.tm_yday);
        return false;
    }

    /* 2000-01-01 00:00:00 UTC, a known Saturday. */
    time_t y2k = 946684800;
    struct tm tm1;
    if (gmtime_r(&y2k, &tm1) == NULL) {
        printf("[selftest] loader/elf_time: gmtime_r(y2k) returned NULL\n");
        return false;
    }
    if (tm1.tm_year != 100 || tm1.tm_mon != 0 || tm1.tm_mday != 1 || tm1.tm_wday != 6 || tm1.tm_yday != 0) {
        printf("[selftest] loader/elf_time: gmtime_r(y2k) mismatch (y=%d m=%d d=%d wd=%d yd=%d)\n",
               tm1.tm_year, tm1.tm_mon, tm1.tm_mday, tm1.tm_wday, tm1.tm_yday);
        return false;
    }

    /* 2000-03-01 00:00:00 UTC: day 60 of a leap year (Jan 31 + Feb 29 days
     * precede it), exercising both the leap-year rule and tm_yday. */
    time_t leap_check = 951868800;
    struct tm tm2;
    gmtime_r(&leap_check, &tm2);
    if (tm2.tm_mon != 2 || tm2.tm_mday != 1 || tm2.tm_yday != 60) {
        printf("[selftest] loader/elf_time: gmtime_r leap-year mismatch (m=%d d=%d yd=%d)\n", tm2.tm_mon,
               tm2.tm_mday, tm2.tm_yday);
        return false;
    }

    /* mktime() must be the exact inverse of localtime_r() for any epoch
     * second, regardless of Config's currently configured offset. */
    time_t now = time(NULL);
    struct tm local;
    if (bruce_elf__localtime_r(&now, &local) == NULL) {
        printf("[selftest] loader/elf_time: localtime_r returned NULL\n");
        return false;
    }
    time_t roundtrip = bruce_elf__mktime(&local);
    if (roundtrip != now) {
        printf("[selftest] loader/elf_time: mktime(localtime_r(t)) != t (%lld != %lld)\n",
               (long long)roundtrip, (long long)now);
        return false;
    }

    /* localtime_r() must actually apply Config's offset -- compare against
     * an independently-computed shift rather than assuming a particular
     * configured timezone/DST value. */
    int64_t expected_offset =
        (int64_t)(((double)config__get_time_timezone() + (config__get_time_dst() ? 1.0 : 0.0)) * 3600.0);
    time_t shifted = now + (time_t)expected_offset;
    struct tm expected_local;
    gmtime_r(&shifted, &expected_local);
    if (local.tm_year != expected_local.tm_year || local.tm_mon != expected_local.tm_mon ||
        local.tm_mday != expected_local.tm_mday || local.tm_hour != expected_local.tm_hour ||
        local.tm_min != expected_local.tm_min || local.tm_sec != expected_local.tm_sec) {
        printf("[selftest] loader/elf_time: localtime_r did not apply Config's offset\n");
        return false;
    }

    /* clock() must at least return without crashing and stay non-decreasing
     * across two back-to-back calls (ignoring the documented ~71-minute
     * wraparound, which a fast selftest can't hit). */
    clock_t c1 = bruce_elf__clock();
    clock_t c2 = bruce_elf__clock();
    if (c2 < c1) {
        printf("[selftest] loader/elf_time: clock() went backwards (%lu -> %lu)\n", (unsigned long)c1,
               (unsigned long)c2);
        return false;
    }

    printf("[selftest] loader/elf_time: OK\n");
    return true;
}

/*
 * Exercises the ELF loader's POSIX file/dir adapters (open/close/read/
 * write/lseek, stat/fstat, mkdir/access/unlink, opendir/readdir/closedir),
 * by calling the (deliberately non-static) adapter functions directly, same
 * rationale as selftest__run_elf_loader_stdio_case(). Also checks that fd
 * 0/1/2 are pre-bound to the same console sentinels the FILE*-based
 * stdin/stdout/stderr use, and that closing them is a no-op.
 */
bool selftest__run_elf_loader_posix_case(void) {
    const char *dir = "/selftest_elf_posix";
    const char *file_a = "/selftest_elf_posix/a.txt";
    const char *file_b = "/selftest_elf_posix/b.txt";
    const char *missing = "/selftest_elf_posix/missing.txt";

    /* Best-effort cleanup from a previous interrupted run. */
    bruce_elf__remove(file_a);
    bruce_elf__remove(file_b);
    bruce_elf__remove(dir);

    if (bruce_elf__mkdir(dir, 0755) != 0) {
        printf("[selftest] loader/elf_posix: mkdir failed\n");
        return false;
    }

    int fd = bruce_elf__open(file_a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[selftest] loader/elf_posix: open(O_WRONLY|O_CREAT) failed (%d)\n", fd);
        return false;
    }
    if (bruce_elf__write(fd, "hello", 5) != 5) {
        printf("[selftest] loader/elf_posix: write mismatch\n");
        return false;
    }
    if (bruce_elf__close(fd) != 0) {
        printf("[selftest] loader/elf_posix: close failed\n");
        return false;
    }

    if (bruce_elf__access(file_a, 0) != 0) {
        printf("[selftest] loader/elf_posix: access on existing file failed\n");
        return false;
    }
    if (bruce_elf__access(missing, 0) == 0) {
        printf("[selftest] loader/elf_posix: access on missing file succeeded\n");
        return false;
    }

    struct stat file_stat;
    if (bruce_elf__stat(file_a, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) || file_stat.st_size != 5) {
        printf("[selftest] loader/elf_posix: stat(file) mismatch\n");
        return false;
    }
    struct stat dir_stat;
    if (bruce_elf__stat(dir, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
        printf("[selftest] loader/elf_posix: stat(dir) mismatch\n");
        return false;
    }

    fd = bruce_elf__open(file_a, O_RDONLY);
    if (fd < 0) {
        printf("[selftest] loader/elf_posix: open(O_RDONLY) failed (%d)\n", fd);
        return false;
    }
    char buffer[16] = {0};
    if (bruce_elf__read(fd, buffer, sizeof(buffer)) != 5 || strcmp(buffer, "hello") != 0) {
        printf("[selftest] loader/elf_posix: read mismatch (\"%s\")\n", buffer);
        return false;
    }
    struct stat fd_stat;
    if (bruce_elf__fstat(fd, &fd_stat) != 0 || fd_stat.st_size != 5) {
        printf("[selftest] loader/elf_posix: fstat mismatch\n");
        return false;
    }
    if (bruce_elf__lseek(fd, 2, SEEK_SET) != 2) {
        printf("[selftest] loader/elf_posix: lseek mismatch\n");
        return false;
    }
    memset(buffer, 0, sizeof(buffer));
    if (bruce_elf__read(fd, buffer, sizeof(buffer)) != 3 || strcmp(buffer, "llo") != 0) {
        printf("[selftest] loader/elf_posix: post-seek read mismatch (\"%s\")\n", buffer);
        return false;
    }
    bruce_elf__close(fd);

    fd = bruce_elf__open(file_b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || bruce_elf__write(fd, "b", 1) != 1) {
        printf("[selftest] loader/elf_posix: creating second file failed\n");
        return false;
    }
    bruce_elf__close(fd);

    DIR *dirp = bruce_elf__opendir(dir);
    if (dirp == NULL) {
        printf("[selftest] loader/elf_posix: opendir failed\n");
        return false;
    }
    bool saw_a = false, saw_b = false;
    struct dirent *entry;
    while ((entry = bruce_elf__readdir(dirp)) != NULL) {
        if (entry->d_type != DT_REG) {
            printf("[selftest] loader/elf_posix: unexpected d_type for \"%s\"\n", entry->d_name);
            bruce_elf__closedir(dirp);
            return false;
        }
        if (strcmp(entry->d_name, "a.txt") == 0) saw_a = true;
        if (strcmp(entry->d_name, "b.txt") == 0) saw_b = true;
    }
    bruce_elf__closedir(dirp);
    if (!saw_a || !saw_b) {
        printf("[selftest] loader/elf_posix: readdir missing an entry (a=%d b=%d)\n", saw_a, saw_b);
        return false;
    }

    /* Bad-fd handling. */
    if (bruce_elf__read(999, buffer, sizeof(buffer)) != -1 || bruce_elf__close(999) != -1) {
        printf("[selftest] loader/elf_posix: bad fd accepted\n");
        return false;
    }

    /* fd 0/1/2 are pre-bound to the same console sentinels FILE*
     * stdin/stdout/stderr use, and closing them is a documented no-op. */
    static const char console_msg[] = "selftest\n";
    if (bruce_elf__write(1, console_msg, sizeof(console_msg) - 1) != (ssize_t)(sizeof(console_msg) - 1)) {
        printf("[selftest] loader/elf_posix: write(1, ...) mismatch\n");
        return false;
    }
    if (bruce_elf__close(1) != 0) {
        printf("[selftest] loader/elf_posix: close(1) failed\n");
        return false;
    }
    if (bruce_elf__write(1, console_msg, sizeof(console_msg) - 1) != (ssize_t)(sizeof(console_msg) - 1)) {
        printf("[selftest] loader/elf_posix: write(1, ...) after close(1) failed -- fd 1 was cleared\n");
        return false;
    }

    bruce_elf__remove(file_a);
    bruce_elf__remove(file_b);
    if (bruce_elf__remove(dir) != 0) {
        printf("[selftest] loader/elf_posix: cleanup rmdir failed\n");
        return false;
    }

    printf("[selftest] loader/elf_posix: OK\n");
    return true;
}

// TODO: Fix elf_loader_test.c
/* native_apps/examples/game.elf, embedded via EMBED_FILES (src/CMakeLists.txt). */
// extern const uint8_t game_elf_start[] asm("_binary_game_elf_start");
// extern const uint8_t game_elf_end[] asm("_binary_game_elf_end");

/*
 * Regression coverage for the flash-backed (XIP) ELF relocation path: stages
 * a real, small, executable ELF app (committed at native_apps/examples/game.elf)
 * and runs it through the exact same AppRunner path dispatch ->
 * esp_elf_relocate_xip() -> memory_external swap allocator pipeline that
 * "elf ./apps/game.elf" uses on real hardware. This is the pipeline that
 * regressed with "flash-backed relocation failed (relocate=-5, release=0)":
 * selftest__run_elf_loader_case()'s fake ELF fixture is intentionally
 * invalid manifest-only bytes and is rejected before relocation is ever
 * attempted, so it cannot catch this class of bug.
 */
bool selftest__run_elf_loader_xip_case(void) {
    const char *path = "/bin/selftest_elf_loader_xip.elf";
    storage__remove(path);

    /* The external fixture writes to storage while running; selftests must
     * never wait for a user to answer its first-use permission prompt. */
    if (permission__set("selftest_elf_loader_xip.elf", BRUCE_PERMISSION_STORAGE, true) != BRUCE_OK) {
        printf("[selftest] loader/elf_xip: could not grant storage permission\n");
        return false;
    }

    // size_t elf_size = (size_t)(game_elf_end - game_elf_start);
    // if (!storage__write_file_atomic(path, game_elf_start, elf_size)) {
    //     printf("[selftest] loader/elf_xip: could not stage embedded fixture\n");
    //     return false;
    // }

    int result = app_runner__run_path(path, NULL, BRUCE_LAUNCH_FOREGROUND);
    if (result > 0) (void)runtime__delay(50);
    storage__remove(path);

    if (result <= 0) {
#if CONFIG_BRUCE_QEMU_TEST_MODE
        if (result == BRUCE_ERR_INVALID_ARGUMENT) {
            printf("[selftest] loader/elf_xip: OK (QEMU relocation unavailable)\n");
            return true;
        }
#endif
        printf("[selftest] loader/elf_xip: open failed (result=%d)\n", result);
        return false;
    }

    printf("[selftest] loader/elf_xip: OK\n");
    return true;
}

bool selftest__run_wasm_loader_case(void) {
    const char *path = "/bin/selftest_wasm_loader_target.wasm";
    storage__remove(path);

    korp_tid task = os_self_thread();
    if (task == 0 || task != os_self_thread()) {
        printf("[selftest] loader/wasm: unstable WAMR task identity\n");
        return false;
    }

    bruce_process_snapshot_t before;
    bruce_process_snapshot_t during;
    bruce_process_snapshot_t after;
    if (process__snapshot(process__current_id(), &before) != BRUCE_OK) {
        printf("[selftest] loader/wasm: could not read initial accounting\n");
        return false;
    }
    void *allocation = wasm_loader__debug_runtime_malloc(4096);
    if (allocation == NULL ||
        process__snapshot(process__current_id(), &during) != BRUCE_OK ||
        during.memory_bytes < before.memory_bytes + 4096u) {
        wasm_loader__debug_runtime_free(allocation);
        printf("[selftest] loader/wasm: WAMR allocation was not accounted\n");
        return false;
    }
    wasm_loader__debug_runtime_free(allocation);
    if (process__snapshot(process__current_id(), &after) != BRUCE_OK ||
        after.memory_bytes != before.memory_bytes) {
        printf("[selftest] loader/wasm: WAMR accounting was not released\n");
        return false;
    }

    size_t calls_before = wasm_loader__debug_call_count();
    int result = app_runner__run_path(path, NULL, BRUCE_LAUNCH_BACKGROUND);
    /* app_runner__run_path() only spawns the "wasm" loader command
     * asynchronously (see app_runner__run_path_with_environment()) -- the
     * wasm loader's own BRUCE_ERR_NOT_FOUND (the target file was never
     * written) is produced inside that spawned process, so it shows up on
     * its exit code, not this call's own return value (just the spawned
     * pid on success). */
    if (result > 0) {
        bruce_process_status_t status;
        result = process__wait_status((bruce_process_id_t)result, 2000, &status) == BRUCE_OK &&
                         status.reason == BRUCE_PROCESS_EXITED
                     ? status.exit_code
                     : result;
    }
    if (result != BRUCE_ERR_NOT_FOUND || wasm_loader__debug_call_count() != calls_before + 1) {
        printf("[selftest] loader/wasm: loader was not dispatched (%d)\n", result);
        return false;
    }

    printf("[selftest] loader/wasm: OK\n");
    return true;
}
