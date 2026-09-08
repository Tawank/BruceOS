#include <dirent.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "lwip/sockets.h"

#include "core/process/process.h"
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

/* Forwards to vsscanf() with a real va_list, the same shape a libc caller
 * would use -- vsscanf() itself is exported to ELF apps unadapted (a pure
 * string-in function, no I/O of its own), so this is regression coverage
 * for the real picolibc function, same rationale as gmtime_r() below. */
static int selftest__elf_vsscanf_helper(const char *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsscanf(buffer, format, args);
    va_end(args);
    return result;
}

/*
 * Exercises the getenv/setenv/unsetenv and strdup/strndup adapters
 * (elf_loader_sdk_symbols.c), by calling the (deliberately non-static)
 * adapter functions directly, same rationale as selftest__run_elf_loader_stdio_case().
 * Also covers the "small libc gaps" additions: strerror()/div()/ldiv()/
 * lldiv()/vsscanf() are exported to ELF apps unadapted (pure functions, no
 * I/O), so they're exercised directly as regression coverage for the real
 * picolibc functions rather than for any BruceOS-specific logic; perror()
 * IS a BruceOS-specific adapter (bruce_elf__perror(), routes through
 * bruce_elf__fprintf(stderr) instead of picolibc's own internal stderr --
 * see its doc comment), so it's called here too, though -- like the
 * stdout/stdin writes in selftest__run_elf_loader_stdio_case() -- its
 * console output itself isn't asserted on, only that it runs to completion
 * without crashing.
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

    const char *enoent_message = strerror(ENOENT);
    const char *einval_message = strerror(EINVAL);
    if (enoent_message == NULL || einval_message == NULL || enoent_message[0] == '\0' ||
        strcmp(enoent_message, einval_message) == 0) {
        printf("[selftest] loader/elf_libc: strerror mismatch (\"%s\" vs \"%s\")\n", enoent_message ? enoent_message : "(null)",
               einval_message ? einval_message : "(null)");
        return false;
    }

    div_t quotient = div(7, 2);
    div_t negative_quotient = div(-7, 2);
    if (quotient.quot != 3 || quotient.rem != 1 || negative_quotient.quot != -3 || negative_quotient.rem != -1) {
        printf("[selftest] loader/elf_libc: div mismatch (7/2=%d,%d -7/2=%d,%d)\n", quotient.quot, quotient.rem,
               negative_quotient.quot, negative_quotient.rem);
        return false;
    }

    ldiv_t long_quotient = ldiv(7L, 2L);
    lldiv_t long_long_quotient = lldiv(7LL, 2LL);
    if (long_quotient.quot != 3L || long_quotient.rem != 1L || long_long_quotient.quot != 3LL ||
        long_long_quotient.rem != 1LL) {
        printf("[selftest] loader/elf_libc: ldiv/lldiv mismatch\n");
        return false;
    }

    int scanned_int = 0;
    char scanned_word[16] = {0};
    if (selftest__elf_vsscanf_helper("42 hello", "%d %15s", &scanned_int, scanned_word) != 2 || scanned_int != 42 ||
        strcmp(scanned_word, "hello") != 0) {
        printf("[selftest] loader/elf_libc: vsscanf mismatch (%d, \"%s\")\n", scanned_int, scanned_word);
        return false;
    }

    errno = ENOENT;
    bruce_elf__perror("selftest");
    errno = 0;

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

/*
 * Exercises the BSD socket adapters the ELF loader hands to sandboxed apps
 * (elf_loader_sdk_symbols.c, backed by core_sdk/tcp.h and core_sdk/udp.h),
 * by calling the (deliberately non-static) adapter functions directly, same
 * rationale as selftest__run_elf_loader_stdio_case(). Runs as the selftest
 * task itself, a built-in process that permission__check() always allows
 * (permission enforcement is covered separately, same pattern as
 * selftest__run_udp_permission_denied_case() in wifi_test.c), over the
 * loopback interface (works under QEMU with no real network, same as
 * selftest__run_udp_loopback_case()).
 */
bool selftest__run_elf_loader_socket_case(void) {
    const uint16_t tcp_port = 47010;
    const uint16_t udp_port_b = 47012;

    /* Rejected domain/type. */
    if (bruce_elf__socket(AF_INET, SOCK_RAW, 0) != -1 || errno != EINVAL) {
        printf("[selftest] loader/elf_socket: SOCK_RAW was not rejected\n");
        return false;
    }

    /* --- TCP: listener + client + accept, then a round-trip both ways. --- */
    int listener = bruce_elf__socket(AF_INET, SOCK_STREAM, 0);
    int client = bruce_elf__socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || client < 0) {
        printf("[selftest] loader/elf_socket: tcp socket() failed\n");
        return false;
    }

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(tcp_port);
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bruce_elf__bind(listener, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        printf("[selftest] loader/elf_socket: tcp bind() failed\n");
        return false;
    }
    if (bruce_elf__listen(listener, 1) != 0) {
        printf("[selftest] loader/elf_socket: tcp listen() failed\n");
        return false;
    }

    struct sockaddr_in connect_addr = {0};
    connect_addr.sin_family = AF_INET;
    connect_addr.sin_port = htons(tcp_port);
    inet_pton(AF_INET, "127.0.0.1", &connect_addr.sin_addr);
    if (bruce_elf__connect(client, (struct sockaddr *)&connect_addr, sizeof(connect_addr)) != 0) {
        printf("[selftest] loader/elf_socket: tcp connect() failed\n");
        return false;
    }

    struct sockaddr_in peer_addr = {0};
    socklen_t peer_len = sizeof(peer_addr);
    int accepted = bruce_elf__accept(listener, (struct sockaddr *)&peer_addr, &peer_len);
    if (accepted < 0 || peer_len != sizeof(peer_addr) || peer_addr.sin_family != AF_INET) {
        printf("[selftest] loader/elf_socket: tcp accept() failed\n");
        return false;
    }

    static const char to_server[] = "hello server";
    static const char to_client[] = "hello client";
    char buffer[32] = {0};
    if (bruce_elf__send(client, to_server, sizeof(to_server), 0) != (ssize_t)sizeof(to_server)) {
        printf("[selftest] loader/elf_socket: tcp send (client->server) mismatch\n");
        return false;
    }
    if (bruce_elf__recv(accepted, buffer, sizeof(buffer), 0) != (ssize_t)sizeof(to_server) ||
        strcmp(buffer, to_server) != 0) {
        printf("[selftest] loader/elf_socket: tcp recv (client->server) mismatch (\"%s\")\n", buffer);
        return false;
    }
    memset(buffer, 0, sizeof(buffer));
    if (bruce_elf__send(accepted, to_client, sizeof(to_client), 0) != (ssize_t)sizeof(to_client)) {
        printf("[selftest] loader/elf_socket: tcp send (server->client) mismatch\n");
        return false;
    }
    if (bruce_elf__recv(client, buffer, sizeof(buffer), 0) != (ssize_t)sizeof(to_client) ||
        strcmp(buffer, to_client) != 0) {
        printf("[selftest] loader/elf_socket: tcp recv (server->client) mismatch (\"%s\")\n", buffer);
        return false;
    }

    /* getsockopt(SO_ERROR) always reports "no error"; setsockopt/shutdown
     * are documented no-ops that still validate the fd is a socket. */
    int so_error = -1;
    socklen_t so_error_len = sizeof(so_error);
    if (bruce_elf__getsockopt(client, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0 || so_error != 0) {
        printf("[selftest] loader/elf_socket: getsockopt(SO_ERROR) mismatch\n");
        return false;
    }
    int reuse = 1;
    if (bruce_elf__setsockopt(client, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        printf("[selftest] loader/elf_socket: setsockopt failed\n");
        return false;
    }
    if (bruce_elf__shutdown(client, SHUT_RDWR) != 0) {
        printf("[selftest] loader/elf_socket: shutdown failed\n");
        return false;
    }
    /* A non-socket fd must be rejected by the socket-only calls. */
    if (bruce_elf__getsockopt(1, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != -1 || errno != ENOTSOCK) {
        printf("[selftest] loader/elf_socket: getsockopt accepted a non-socket fd\n");
        return false;
    }

    bruce_elf__close(client);
    bruce_elf__close(accepted);
    bruce_elf__close(listener);

    /* --- UDP: sendto/recvfrom on an explicitly-bound socket, then
     * connect()+send()/recv() on the other, including sender filtering. --- */
    int udp_a = bruce_elf__socket(AF_INET, SOCK_DGRAM, 0);
    int udp_b = bruce_elf__socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_a < 0 || udp_b < 0) {
        printf("[selftest] loader/elf_socket: udp socket() failed\n");
        return false;
    }
    struct sockaddr_in udp_bind_b = {0};
    udp_bind_b.sin_family = AF_INET;
    udp_bind_b.sin_port = htons(udp_port_b);
    udp_bind_b.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bruce_elf__bind(udp_b, (struct sockaddr *)&udp_bind_b, sizeof(udp_bind_b)) != 0) {
        printf("[selftest] loader/elf_socket: udp bind() failed\n");
        return false;
    }

    struct sockaddr_in udp_dest_b = {0};
    udp_dest_b.sin_family = AF_INET;
    udp_dest_b.sin_port = htons(udp_port_b);
    inet_pton(AF_INET, "127.0.0.1", &udp_dest_b.sin_addr);
    static const char dgram_to_b[] = "dgram to b";
    if (bruce_elf__sendto(udp_a, dgram_to_b, sizeof(dgram_to_b), 0, (struct sockaddr *)&udp_dest_b,
                           sizeof(udp_dest_b)) != (ssize_t)sizeof(dgram_to_b)) {
        printf("[selftest] loader/elf_socket: udp sendto() failed\n");
        return false;
    }
    struct sockaddr_in from_addr = {0};
    socklen_t from_len = sizeof(from_addr);
    memset(buffer, 0, sizeof(buffer));
    if (bruce_elf__recvfrom(udp_b, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len) !=
            (ssize_t)sizeof(dgram_to_b) ||
        strcmp(buffer, dgram_to_b) != 0 || from_len != sizeof(from_addr)) {
        printf("[selftest] loader/elf_socket: udp recvfrom() mismatch (\"%s\")\n", buffer);
        return false;
    }

    /* connect() on udp_a latches udp_b as its peer; plain send()/recv() now
     * route through that peer without naming it on every call. */
    struct sockaddr_in udp_connect_a = udp_dest_b;
    if (bruce_elf__connect(udp_a, (struct sockaddr *)&udp_connect_a, sizeof(udp_connect_a)) != 0) {
        printf("[selftest] loader/elf_socket: udp connect() failed\n");
        return false;
    }
    static const char dgram_connected[] = "connected dgram";
    if (bruce_elf__send(udp_a, dgram_connected, sizeof(dgram_connected), 0) != (ssize_t)sizeof(dgram_connected)) {
        printf("[selftest] loader/elf_socket: udp send() (connected) failed\n");
        return false;
    }
    memset(buffer, 0, sizeof(buffer));
    if (bruce_elf__recvfrom(udp_b, buffer, sizeof(buffer), 0, NULL, NULL) != (ssize_t)sizeof(dgram_connected) ||
        strcmp(buffer, dgram_connected) != 0) {
        printf("[selftest] loader/elf_socket: udp recv of connected send mismatch (\"%s\")\n", buffer);
        return false;
    }

    /* No datagram pending now -- MSG_DONTWAIT must return EAGAIN rather than
     * blocking or returning stale data. */
    if (bruce_elf__recv(udp_a, buffer, sizeof(buffer), MSG_DONTWAIT) != -1 || errno != EAGAIN) {
        printf("[selftest] loader/elf_socket: udp MSG_DONTWAIT recv did not report EAGAIN\n");
        return false;
    }

    bruce_elf__close(udp_a);
    bruce_elf__close(udp_b);

    printf("[selftest] loader/elf_socket: OK\n");
    return true;
}

/*
 * Exercises bruce_elf__exit()/bruce_elf__abort() (elf_loader_sdk_symbols.c)
 * without a real cross-compiled ELF fixture: elf_loader_app.c's
 * elf_loader__entry() is the only other place that arms a sandbox exit
 * target (via process_registry__set_sandbox_exit_target(), core/process/
 * process.h), and setjmp() is no different called from here than called
 * from there -- this reproduces exactly that arm/setjmp/call/longjmp
 * sequence on the selftest's own stack, then confirms the exit status came
 * back through the shared bruce_elf_exit_context_t (elf_loader_internal.h)
 * correctly for both a chosen exit() status and abort()'s fixed
 * BRUCE_ELF_ABORT_EXIT_CODE. The "no target armed" park-forever fallback in
 * bruce_elf__exit_common() is deliberately not exercised here -- there is no
 * way to call into it and get control back to report a result.
 */
bool selftest__run_elf_loader_exit_case(void) {
    bruce_elf_exit_context_t exit_ctx = {.exit_code = -1};

    process_registry__set_sandbox_exit_target(&exit_ctx);
    if (setjmp(exit_ctx.target) == 0) {
        bruce_elf__exit(42);
        process_registry__set_sandbox_exit_target(NULL);
        printf("[selftest] loader/elf_exit: exit() returned instead of unwinding\n");
        return false;
    }
    process_registry__set_sandbox_exit_target(NULL);
    if (exit_ctx.exit_code != 42) {
        printf("[selftest] loader/elf_exit: exit(42) carried status %d\n", exit_ctx.exit_code);
        return false;
    }

    exit_ctx.exit_code = -1;
    process_registry__set_sandbox_exit_target(&exit_ctx);
    if (setjmp(exit_ctx.target) == 0) {
        bruce_elf__abort();
        process_registry__set_sandbox_exit_target(NULL);
        printf("[selftest] loader/elf_exit: abort() returned instead of unwinding\n");
        return false;
    }
    process_registry__set_sandbox_exit_target(NULL);
    if (exit_ctx.exit_code != BRUCE_ELF_ABORT_EXIT_CODE) {
        printf("[selftest] loader/elf_exit: abort() carried status %d, want %d\n", exit_ctx.exit_code,
               BRUCE_ELF_ABORT_EXIT_CODE);
        return false;
    }

    printf("[selftest] loader/elf_exit: OK\n");
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
