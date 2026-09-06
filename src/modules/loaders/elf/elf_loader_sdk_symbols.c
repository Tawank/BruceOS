/* Public SDK symbol table exported to ELF applications.
 *
 * The ELF loader module registers this table with the Espressif ELF loader and
 * uses a custom resolver that searches only these symbols. Selected libc names
 * are mapped to process-aware SDK functions; all other unknown symbols resolve to
 * 0 and cause relocation failure, which is the desired sandbox behavior.
 *
 * When adding a new public SDK capability, also export its entry points here
 * if ELF apps are expected to call them directly.
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_elf.h" // IWYU pragma: export

#include "core_sdk/app_runner.h"
#include "core_sdk/args.h"
#include "core_sdk/audio.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/bluetooth_hid.h"
#include "core_sdk/clock.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/dialog.h"
#include "core_sdk/disk.h"
#include "core_sdk/display.h"
#include "core_sdk/environment.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/gpio.h"
#include "core_sdk/http.h"
#include "core_sdk/i2c.h"
#include "core_sdk/icon.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/ir.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/notification.h"
#include "core_sdk/nrf24.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/pubsub.h"
#include "core_sdk/runtime.h"
#include "core_sdk/spi.h"
#include "core_sdk/ssh.h"
#include "core_sdk/status_icon.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tcp.h"
#include "core_sdk/tty.h"
#include "core_sdk/wifi.h"

/* GCC emits these libgcc helpers for floating-point operations in ELF apps.
 * Keep them in the restricted resolver so portable C code does not need to
 * carry target-specific libgcc objects inside every loadable image. */
extern int __eqdf2(double left, double right);
extern double __adddf3(double left, double right);
extern long long __divdi3(long long dividend, long long divisor);
extern int *__errno(void);
extern float __divsf3(float left, float right);
extern float __addsf3(float left, float right);
extern float __subsf3(float left, float right);
extern float __mulsf3(float left, float right);
extern float __floatsisf(int value);
extern int __fixsfsi(float value);
extern unsigned int __fixunssfsi(float value);
extern int __gesf2(float left, float right);
extern int __ltsf2(float left, float right);
extern long long __fixdfdi(double value);
extern double __floatsidf(int value);
extern float __floatdisf(long long value);
extern double __floatdidf(long long value);
extern double __floatundidf(unsigned long long value);
extern float __floatundisf(unsigned long long value);
extern double __floatunsidf(unsigned int value);
extern double __extendsfdf2(float value);
extern int __fixdfsi(double value);
extern unsigned int __fixunsdfsi(double value);
extern unsigned long long __fixunsdfdi(double value);
extern long long __fixsfdi(float value);
extern unsigned long long __fixunssfdi(float value);
extern int __gedf2(double left, double right);
extern int __gtdf2(double left, double right);
extern int __ledf2(double left, double right);
extern int __ltdf2(double left, double right);
extern long long __moddi3(long long dividend, long long divisor);
extern int __nedf2(double left, double right);
extern int __unorddf2(double left, double right);
extern unsigned long long __umoddi3(unsigned long long dividend, unsigned long long divisor);
extern double __divdf3(double left, double right);
extern double __muldf3(double left, double right);
extern double __subdf3(double left, double right);
extern float __truncdfsf2(double value);
extern unsigned long long __udivdi3(unsigned long long dividend, unsigned long long divisor);
extern float __ieee754_sqrtf(float value);

static int bruce_elf__puts(const char *text) {
    if (text == NULL || stdio__write(text, strlen(text)) != BRUCE_OK) return EOF;
    return stdio__write("\n", 1) == BRUCE_OK ? 0 : EOF;
}

static int bruce_elf__putchar(int character) {
    unsigned char byte = (unsigned char)character;
    return stdio__write(&byte, 1) == BRUCE_OK ? byte : EOF;
}

/* ---------------------------------------------------------------------------
 * FILE*-based stdio, backed by storage__open/read/write/seek/close (see
 * core_sdk/storage.h). `bruce_file_id_t` is deliberately opaque (an index
 * into storage.c's own slot table, behind which live permission checks,
 * per-process auto-close-on-kill, and SD/mount bookkeeping -- see that
 * header's own doc comment) so this doesn't try to "become" a `FILE*` or
 * vice versa; instead every `FILE*` handed back here is a small heap box
 * the loaded ELF app only ever passes back into these same adapters, never
 * dereferences itself -- the exact opacity contract real <stdio.h> already
 * gives any conforming caller.
 *
 * Not declared `static` (unlike every adapter above/below) so
 * modules/selftest -- which already gets a core-header exemption under this
 * project's module-boundary rule -- can call them directly to exercise the
 * mode-string parsing, flag mapping, and eof/error tracking below without
 * needing a full cross-compiled ELF fixture; see
 * elf_loader_sdk_symbols_test.h.
 *
 * getc/putc alias fgetc/fputc directly (no separate adapter -- same
 * int(FILE*)/int(int,FILE*) signature). This project's actual toolchain
 * library -- checked via `xtensa-esp32s3-elf-gcc -H`, which is picolibc
 * here, not newlib (worth confirming directly rather than assuming from
 * the toolchain's directory name) -- declares getc() as a plain function
 * and defines putc() as `#define putc(c, stream) fputc(c, stream)`: a
 * straight call-through, not a macro that pokes a real FILE struct's
 * internal fields the way some other libc's getc/putc do. Either spelling
 * is equally safe against this opaque box on this toolchain.
 *
 * Deliberately NOT provided: tmpfile/tmpnam (no sane directory/cleanup
 * story on this filesystem), freopen, and ungetc (needs a pushback buffer
 * per FILE).
 *
 * stdin/stdout/stderr are exported too (see the two static sentinel boxes
 * and the bruce_elf__std*_ptr variables below), which is why `kind` exists
 * on the box at all: picolibc declares these as real `extern FILE *stdin;`
 * globals holding a pointer, not compile-time constants, so a relocation
 * against the symbol "stdout" resolves to the address of a FILE* variable
 * whose *value* is this table's answer -- if that value were the real
 * picolibc stdout (a pointer into ITS OWN internal FILE struct, a
 * completely different layout from bruce_elf_file_t), any of the adapters
 * above would misread that struct's bytes as this box's fields instead of
 * failing cleanly. Routing storage__ vs stdio__ by `kind` keeps a single
 * fprintf()/fwrite()/fgets() implementation correct for both a real file
 * and the console, exactly like a real libc's FILE does internally. */
typedef enum {
    BRUCE_ELF_FILE_STORAGE,
    BRUCE_ELF_FILE_CONSOLE_IN,
    BRUCE_ELF_FILE_CONSOLE_OUT,
} bruce_elf_file_kind_t;

typedef struct {
    bruce_elf_file_kind_t kind;
    bruce_file_id_t file; /* meaningful only when kind == BRUCE_ELF_FILE_STORAGE */
    bool eof;
    bool error;
} bruce_elf_file_t;

/* Static, never freed -- fclose() on one of these is a no-op (see below),
 * matching fclose(stdout) being harmless in a real libc. stderr shares
 * stdout's box: BruceOS's console model is a single per-process output
 * stream (stdio__write), there is no separate error stream to route to. */
static bruce_elf_file_t g_bruce_elf_console_in = {.kind = BRUCE_ELF_FILE_CONSOLE_IN};
static bruce_elf_file_t g_bruce_elf_console_out = {.kind = BRUCE_ELF_FILE_CONSOLE_OUT};

/* Not `static`: these back the "stdin"/"stdout"/"stderr" table entries.
 * Each is a real FILE* variable (not the FILE itself) because that is
 * exactly picolibc's own `extern FILE *stdout;` shape -- the symbol names
 * an object that *holds* a pointer, not the pointed-to object. */
FILE *bruce_elf__stdin_ptr = (FILE *)&g_bruce_elf_console_in;
FILE *bruce_elf__stdout_ptr = (FILE *)&g_bruce_elf_console_out;
FILE *bruce_elf__stderr_ptr = (FILE *)&g_bruce_elf_console_out;

static void bruce_elf__set_errno_from_result(bruce_result_t result) {
    switch (result) {
        case BRUCE_ERR_NOT_FOUND: *__errno() = ENOENT; break;
        case BRUCE_ERR_PERMISSION: *__errno() = EACCES; break;
        case BRUCE_ERR_INVALID_PATH:
        case BRUCE_ERR_INVALID_ARGUMENT: *__errno() = EINVAL; break;
        case BRUCE_ERR_ALREADY_EXISTS: *__errno() = EEXIST; break;
        case BRUCE_ERR_RESOURCE_LIMIT: *__errno() = EMFILE; break;
        default: *__errno() = EIO; break;
    }
}

/* Parses a "r"/"w"/"a" plus optional "+" fopen() mode (a trailing "b"/"t" is
 * accepted and ignored -- this filesystem makes no such distinction) into
 * storage__open()'s flags. Returns false (leaving *out_flags untouched) for
 * anything else, matching fopen()'s own "invalid mode" contract. */
static bool bruce_elf__parse_fopen_mode(const char *mode, uint32_t *out_flags) {
    if (mode == NULL || mode[0] == '\0') return false;
    bool plus = false;
    for (const char *c = mode + 1; *c != '\0'; ++c) {
        if (*c == '+') plus = true;
        else if (*c != 'b' && *c != 't') return false;
    }
    switch (mode[0]) {
        case 'r': *out_flags = BRUCE_STORAGE_OPEN_READ | (plus ? BRUCE_STORAGE_OPEN_WRITE : 0u); return true;
        case 'w':
            *out_flags = BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE |
                         (plus ? BRUCE_STORAGE_OPEN_READ : 0u);
            return true;
        case 'a':
            *out_flags = BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_APPEND | BRUCE_STORAGE_OPEN_CREATE |
                         (plus ? BRUCE_STORAGE_OPEN_READ : 0u);
            return true;
        default: return false;
    }
}

FILE *bruce_elf__fopen(const char *path, const char *mode) {
    uint32_t flags;
    if (path == NULL || !bruce_elf__parse_fopen_mode(mode, &flags)) {
        *__errno() = EINVAL;
        return NULL;
    }
    bruce_elf_file_t *box = memory__malloc(sizeof(*box));
    if (box == NULL) {
        *__errno() = ENOMEM;
        return NULL;
    }
    bruce_result_t result = storage__open(path, flags, &box->file);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        memory__free(box);
        return NULL;
    }
    box->kind = BRUCE_ELF_FILE_STORAGE;
    box->eof = false;
    box->error = false;
    return (FILE *)box;
}

int bruce_elf__fclose(FILE *stream) {
    if (stream == NULL) return EOF;
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    if (box->kind != BRUCE_ELF_FILE_STORAGE) return 0; /* static sentinel, not ours to free */
    bruce_result_t result = storage__close(box->file);
    memory__free(box);
    return result == BRUCE_OK ? 0 : EOF;
}

size_t bruce_elf__fread(void *ptr, size_t size, size_t count, FILE *stream) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    if (ptr == NULL || box == NULL || size == 0 || count == 0) return 0;
    if (box->kind == BRUCE_ELF_FILE_CONSOLE_OUT) {
        box->error = true; /* reading from an output-only stream */
        return 0;
    }
    if (box->kind == BRUCE_ELF_FILE_CONSOLE_IN) {
        size_t received = 0;
        bruce_result_t result = stdio__read(ptr, size * count, UINT32_MAX, &received);
        if (result != BRUCE_OK) {
            box->error = true;
            return 0;
        }
        /* Unlike the storage case below, a short read here just means
         * that's everything currently queued on a live console -- not
         * end-of-input -- so only a truly empty read counts as eof. */
        if (received == 0) box->eof = true;
        return received / size;
    }
    size_t received = 0;
    if (storage__read(box->file, ptr, size * count, &received) != BRUCE_OK) {
        box->error = true;
        return 0;
    }
    if (received < size * count) box->eof = true;
    return received / size;
}

size_t bruce_elf__fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    if (ptr == NULL || box == NULL || size == 0 || count == 0) return 0;
    if (box->kind == BRUCE_ELF_FILE_CONSOLE_IN) {
        box->error = true; /* writing to an input-only stream */
        return 0;
    }
    if (box->kind == BRUCE_ELF_FILE_CONSOLE_OUT) {
        if (stdio__write(ptr, size * count) != BRUCE_OK) {
            box->error = true;
            return 0;
        }
        return count;
    }
    size_t written = 0;
    if (storage__write(box->file, ptr, size * count, &written) != BRUCE_OK) {
        box->error = true;
        return 0;
    }
    return written / size;
}

int bruce_elf__fseek(FILE *stream, long offset, int whence) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    if (box == NULL) return -1;
    if (box->kind != BRUCE_ELF_FILE_STORAGE) {
        *__errno() = ESPIPE; /* the console is not seekable */
        return -1;
    }
    if (storage__seek(box->file, offset, whence, NULL) != BRUCE_OK) return -1;
    box->eof = false;
    return 0;
}

long bruce_elf__ftell(FILE *stream) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    if (box == NULL) return -1;
    if (box->kind != BRUCE_ELF_FILE_STORAGE) {
        *__errno() = ESPIPE;
        return -1;
    }
    uint64_t position = 0;
    if (storage__seek(box->file, 0, SEEK_CUR, &position) != BRUCE_OK) return -1;
    return (long)position;
}

void bruce_elf__rewind(FILE *stream) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    if (box == NULL || box->kind != BRUCE_ELF_FILE_STORAGE) return; /* console: nothing to rewind */
    if (storage__seek(box->file, 0, SEEK_SET, NULL) == BRUCE_OK) {
        box->eof = false;
        box->error = false;
    }
}

/* storage__write() is already unbuffered at this layer (a thin wrapper over
 * a real fd -- see storage.c), so there is nothing to flush; this satisfies
 * the contract rather than faking one. */
int bruce_elf__fflush(FILE *stream) {
    (void)stream;
    return 0;
}

/* Provided only so a defensive caller that always calls setvbuf() doesn't
 * get an unresolved-symbol relocation failure -- same unbuffered rationale
 * as fflush() above, so any requested mode/buffer is silently accepted and
 * ignored. */
int bruce_elf__setvbuf(FILE *stream, char *buffer, int mode, size_t size) {
    (void)stream;
    (void)buffer;
    (void)mode;
    (void)size;
    return 0;
}

int bruce_elf__fgetc(FILE *stream) {
    unsigned char byte;
    return bruce_elf__fread(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int bruce_elf__fputc(int character, FILE *stream) {
    unsigned char byte = (unsigned char)character;
    return bruce_elf__fwrite(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

char *bruce_elf__fgets(char *buffer, int size, FILE *stream) {
    if (buffer == NULL || size <= 0) return NULL;
    int written = 0;
    while (written < size - 1) {
        int character = bruce_elf__fgetc(stream);
        if (character == EOF) break;
        buffer[written++] = (char)character;
        if (character == '\n') break;
    }
    if (written == 0) return NULL; /* EOF/error before any byte was read */
    buffer[written] = '\0';
    return buffer;
}

int bruce_elf__fputs(const char *text, FILE *stream) {
    if (text == NULL) return EOF;
    size_t length = strlen(text);
    return length == 0 || bruce_elf__fwrite(text, 1, length, stream) == length ? 0 : EOF;
}

int bruce_elf__feof(FILE *stream) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    return box != NULL && box->eof ? 1 : 0;
}

int bruce_elf__ferror(FILE *stream) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    return box != NULL && box->error ? 1 : 0;
}

void bruce_elf__clearerr(FILE *stream) {
    bruce_elf_file_t *box = (bruce_elf_file_t *)stream;
    if (box == NULL) return;
    box->eof = false;
    box->error = false;
}

int bruce_elf__remove(const char *path) {
    bruce_result_t result = storage__remove(path);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int bruce_elf__rename(const char *from, const char *to) {
    bruce_result_t result = storage__rename(from, to);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    return 0;
}

/* Sizes the formatted output exactly via a NULL/0 vsnprintf() dry run, then
 * formats into a heap buffer of that exact size and writes it out whole --
 * no truncation, unlike a fixed-size stack buffer. */
int bruce_elf__vfprintf(FILE *stream, const char *format, va_list args) {
    va_list length_args;
    va_copy(length_args, args);
    int needed = vsnprintf(NULL, 0, format, length_args);
    va_end(length_args);
    if (needed < 0) return -1;
    char *buffer = memory__malloc((size_t)needed + 1u);
    if (buffer == NULL) {
        *__errno() = ENOMEM;
        return -1;
    }
    int written = vsnprintf(buffer, (size_t)needed + 1u, format, args);
    bool ok = written == needed && bruce_elf__fwrite(buffer, 1, (size_t)needed, stream) == (size_t)needed;
    memory__free(buffer);
    return ok ? written : -1;
}

int bruce_elf__fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = bruce_elf__vfprintf(stream, format, args);
    va_end(args);
    return result;
}

/* getenv/setenv/unsetenv, backed by the calling process's own environment__
 * (core_sdk/environment.h) -- runtime-only, inherited as a deep copy by
 * child processes, exactly matching POSIX getenv/setenv/unsetenv scope.
 * Like the FILE* family above, not declared `static` so modules/selftest
 * can call these (and strdup/strndup below) directly; see
 * elf_loader_sdk_symbols_test.h. */
char *bruce_elf__getenv(const char *name) {
    return (char *)environment__get(name);
}

int bruce_elf__setenv(const char *name, const char *value, int overwrite) {
    if (name == NULL || value == NULL) {
        *__errno() = EINVAL;
        return -1;
    }
    if (!overwrite && environment__get(name) != NULL) return 0;
    bruce_result_t result = environment__set(name, value);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int bruce_elf__unsetenv(const char *name) {
    bruce_result_t result = environment__unset(name);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    return 0;
}

/* strdup/strndup, heap-allocated through memory__malloc rather than the
 * firmware's own real strdup (which would call real malloc internally) --
 * same per-process accounting rationale as malloc/free/calloc/realloc
 * above; a caller frees the result with the aliased free() like any other
 * memory__malloc() allocation. */
char *bruce_elf__strdup(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = memory__malloc(length);
    if (copy == NULL) {
        *__errno() = ENOMEM;
        return NULL;
    }
    memcpy(copy, text, length);
    return copy;
}

char *bruce_elf__strndup(const char *text, size_t size) {
    if (text == NULL) return NULL;
    size_t length = strnlen(text, size);
    char *copy = memory__malloc(length + 1);
    if (copy == NULL) {
        *__errno() = ENOMEM;
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* assert(): its __assert_func(file, line, func, expr) call on failure (and
 * the older three-argument __assert(file, line, msg) some code still calls
 * directly) has nowhere sane to return to -- there is no exception/unwind
 * support here (see the C++ ABI comment block below) -- so, like
 * __cxa_pure_virtual, print and park rather than returning into whatever a
 * real abort() would have done. */
static void bruce_elf__assert_func(const char *file, int line, const char *func, const char *expr) {
    stdio__printf(
        "bruce: assertion \"%s\" failed: file \"%s\", line %d%s%s\n", expr ? expr : "?", file ? file : "?", line,
        func ? ", function: " : "", func ? func : ""
    );
    for (;;) { runtime__delay(1000); }
}

static void bruce_elf__assert(const char *file, int line, const char *msg) {
    bruce_elf__assert_func(file, line, NULL, msg);
}

/* ---------------------------------------------------------------------------
 * time.h. time/gmtime/gmtime_r/difftime/strftime/asctime/asctime_r are
 * exported directly, unadapted (see the table entries below): the real
 * picolibc time() already returns the correct UTC epoch on this firmware --
 * it's the exact call clock__get_utc() itself makes (core/clock/clock.c) --
 * and gmtime/gmtime_r/difftime/strftime/asctime/asctime_r have no
 * clock/timezone state of their own to get wrong, so the real functions are
 * exactly correct as-is, same rationale as the libm block above. time_t is
 * 64-bit on this toolchain (checked: __SIZEOF_LONG__ == 4 here, so
 * picolibc's own sys/_types.h selects __int_least64_t over `long`), so
 * there is no Y2038 problem to inherit.
 *
 * localtime/localtime_r/mktime DO need adapting: picolibc has no tzset()/TZ
 * environment support here, so the real functions would just be UTC. These
 * instead go through clock__get_local_offset_seconds() and
 * clock__datetime_to_epoch() (core_sdk/clock.h) -- the exact same
 * Config-driven offset and calendar math core/clock/clock.c itself uses for
 * clock__get_local()/set_local(), rather than a second, separately
 * maintained implementation of the same math living only here.
 *
 * ctime/ctime_r are deliberately not provided: they're specified as
 * asctime(localtime(t)), and the real picolibc ctime() would call its own
 * internal localtime() rather than the adapter above, silently producing a
 * UTC-assuming result inconsistent with what an app's own explicit
 * localtime() call gets -- not worth a whole extra pair of adapters for a
 * function strftime() already fully subsumes. */

/* Folds an out-of-range struct tm month (any int, as mktime() must accept)
 * into tm_year, leaving a month in [1,12] for clock__datetime_to_epoch(),
 * which -- like the real days-in-civil-calendar math it wraps -- requires a
 * normalized month. */
static void bruce_elf__normalize_month(int64_t *year, int *month_1based) {
    int64_t m0 = *month_1based - 1;
    int64_t y = *year + m0 / 12;
    int64_t m = m0 % 12;
    if (m < 0) {
        m += 12;
        y -= 1;
    }
    *year = y;
    *month_1based = (int)m + 1;
}

struct tm *bruce_elf__localtime_r(const time_t *timer, struct tm *out) {
    if (timer == NULL || out == NULL) return NULL;
    time_t shifted = (time_t)(*timer + clock__get_local_offset_seconds());
    if (gmtime_r(&shifted, out) == NULL) return NULL;
    out->tm_isdst = config__get_time_dst() ? 1 : 0;
    return out;
}

struct tm *bruce_elf__localtime(const time_t *timer) {
    static struct tm result; /* matches real localtime()'s non-reentrant contract */
    return bruce_elf__localtime_r(timer, &result);
}

time_t bruce_elf__mktime(struct tm *tm) {
    if (tm == NULL) return (time_t)-1;
    int64_t year = tm->tm_year + 1900;
    int month = tm->tm_mon + 1;
    bruce_elf__normalize_month(&year, &month);
    bruce_clock_datetime_t local = {
        .year = (uint16_t)year,
        .month = (uint8_t)month,
        .day = (uint8_t)tm->tm_mday,
        .hour = (uint8_t)tm->tm_hour,
        .minute = (uint8_t)tm->tm_min,
        .second = (uint8_t)tm->tm_sec,
    };
    int64_t local_epoch;
    if (clock__datetime_to_epoch(&local, &local_epoch) != BRUCE_OK) return (time_t)-1;
    time_t result = (time_t)(local_epoch - clock__get_local_offset_seconds());
    bruce_elf__localtime_r(&result, tm); /* POSIX: also normalize the caller's fields */
    return result;
}

/* Approximates CPU time with wall-clock-since-boot (runtime__now(), which
 * is explicitly documented as "not wall-clock time" and meant only for
 * differences -- see core_sdk/runtime.h): correct for the overwhelmingly
 * common `(clock() - start) / CLOCKS_PER_SEC` elapsed-time idiom, wrong for
 * code that assumes it measures actual CPU time or resets per-process.
 * clock_t is 32-bit unsigned on this toolchain and CLOCKS_PER_SEC is
 * 1000000, so this also wraps roughly every 71 minutes of uptime -- an
 * existing real-world clock() limitation on any 32-bit-clock_t libc, not
 * something this adapter introduces. */
clock_t bruce_elf__clock(void) {
    return (clock_t)(runtime__now() * (CLOCKS_PER_SEC / 1000ULL));
}

/* ---------------------------------------------------------------------------
 * Minimal C++ ABI support, added for C++ ELF apps (see
 * native_apps/examples/game3d, the first one). project_elf() builds ELF apps
 * with `-nostdlib` (see components/elf_loader/elf_loader.cmake) and never
 * links libstdc++/libsupc++, so a C++ app leaves every C++ runtime symbol --
 * even plain `new`/`delete` -- as an unresolved relocation for this loader to
 * satisfy at load time, exactly like malloc/free above.
 *
 * Only the bare minimum is provided: freestanding global operator new/delete
 * (routed through the same process-aware memory__* allocator as malloc/free)
 * and a __cxa_pure_virtual trap. There is deliberately no support for
 * exceptions (__cxa_throw, __gxx_personality_v0, ...), RTTI (typeinfo,
 * dynamic_cast), thread-safe function-local statics (__cxa_guard_*), or
 * global/namespace-scope objects with non-trivial constructors (this loader
 * never walks .init_array -- grep esp_elf.c). A C++ ELF app's own build must
 * therefore compile with -fno-exceptions -fno-rtti -fno-threadsafe-statics
 * and avoid namespace-scope objects with non-trivial constructors; see
 * native_apps/examples/game3d/main/CMakeLists.txt for the flags and
 * native_apps/examples/game3d/README.md for the reasoning.
 *
 * The symbol names below are the Itanium C++ ABI mangling for global
 * operator new/delete on the 32-bit targets this project builds for
 * (size_t == unsigned int, mangled 'j'); a 64-bit target would need the 'm'
 * forms instead. Like malloc/free, a failed allocation returns NULL rather
 * than throwing (there is no exception support to throw with, and nothing
 * in the ELF apps built against this table checks new's return value).
 * ------------------------------------------------------------------------- */
static void *bruce_elf__operator_new(size_t size) { return memory__malloc(size); }
static void bruce_elf__operator_delete(void *ptr) { memory__free(ptr); }
static void bruce_elf__operator_delete_sized(void *ptr, size_t size) {
    (void)size;
    memory__free(ptr);
}
/* A function-local `static` object with a non-trivial destructor (e.g. a
 * std::vector -- see Jet's Scene.cpp, a dependency of
 * native_apps/examples/game3d, the first C++ ELF app) still gets its destructor
 * registered via __cxa_atexit even under -fno-threadsafe-statics, which only
 * suppresses the *initialization* guard (__cxa_guard_*), not this. Real
 * __cxa_atexit registers a destructor to run when the process exits via a
 * full C runtime exit() call; ELF apps have no such teardown path (app_main
 * just returns and the loader reclaims the process's memory directly -- see
 * the .init_array comment above), so there is nothing useful to register.
 * A no-op that reports success satisfies the ABI contract without pretending
 * to actually run anything later.
 *
 * Note this covers __cxa_atexit itself but deliberately not its companion
 * __dso_handle: that symbol has hidden ELF visibility by ABI convention, so
 * project_elf()'s -fPIC -shared link refuses to leave it as a runtime-
 * resolved external the way it does every symbol in this table -- it must
 * be defined inside the app's own linked objects instead. See game3d's
 * main.cpp for that definition and a fuller explanation. */
static int bruce_elf__cxa_atexit(void (*destructor)(void *), void *arg, void *dso_handle) {
    (void)destructor;
    (void)arg;
    (void)dso_handle;
    return 0;
}

static void bruce_elf__cxa_pure_virtual(void) {
    static const char message[] = "bruce: pure virtual function call\n";
    stdio__write(message, sizeof(message) - 1);
    /* Should never be reached (see the comment block above); park the
     * process instead of falling through into whatever garbage the caller
     * expected a real override to return. runtime__delay() keeps this from
     * spinning the watchdog while parked. */
    for (;;) { runtime__delay(1000); }
}

/* libstdc++'s <vector>/<new> headers call these on genuinely exceptional
 * conditions (allocation failure, a requested container size past its
 * implementation limit, an overflowing array-new size computation) even
 * under -fno-exceptions -- the header always calls the out-of-line
 * function; whether it throws or aborts is that function's own decision,
 * normally made inside libstdc++.a, which this table stands in for. Since
 * there is no exception support to throw with here (see the comment block
 * above), park the same way __cxa_pure_virtual does -- one of these firing
 * means a real bug (OOM, or a size that should never have been requested),
 * not a recoverable condition. Mangled names: Itanium C++ ABI for
 * std::__throw_bad_alloc(), std::__throw_length_error(char const*), and
 * std::__throw_bad_array_new_length(). */
static void bruce_elf__throw_bad_alloc(void) {
    static const char message[] = "bruce: std::__throw_bad_alloc\n";
    stdio__write(message, sizeof(message) - 1);
    for (;;) { runtime__delay(1000); }
}
static void bruce_elf__throw_length_error(const char *what) {
    stdio__write("bruce: std::__throw_length_error: ", 35);
    if (what != NULL) stdio__write(what, strlen(what));
    stdio__write("\n", 1);
    for (;;) { runtime__delay(1000); }
}
static void bruce_elf__throw_bad_array_new_length(void) {
    static const char message[] = "bruce: std::__throw_bad_array_new_length\n";
    stdio__write(message, sizeof(message) - 1);
    for (;;) { runtime__delay(1000); }
}

/* operator new(size_t, const std::nothrow_t&): returning NULL on failure is
 * exactly the nothrow contract, so this is just operator new without the
 * (absent) throwing behaviour -- no need to see the actual std::nothrow_t
 * tag object, its type is never inspected. Mangled name: 32-bit size_t
 * ('j'), matching the plain operator new forms above. */
static void *bruce_elf__operator_new_nothrow(size_t size, const void *tag) {
    (void)tag;
    return memory__malloc(size);
}

const struct esp_elfsym g_bruce_sdk_elfsyms[] = {
    /* Result descriptions */
    ESP_ELFSYM_EXPORT(result__to_string),

    /* Core runtime / process */
    ESP_ELFSYM_EXPORT(runtime__now),
    ESP_ELFSYM_EXPORT(runtime__sleep),
    ESP_ELFSYM_EXPORT(runtime__delay),
    ESP_ELFSYM_EXPORT(runtime__timer_start),
    ESP_ELFSYM_EXPORT(runtime__timer_wait),
    ESP_ELFSYM_EXPORT(runtime__timer_stop),
    ESP_ELFSYM_EXPORT(runtime__gui_requested),
    ESP_ELFSYM_EXPORT(runtime__to_foreground),
    ESP_ELFSYM_EXPORT(runtime__to_background),
    ESP_ELFSYM_EXPORT(process__current_id),
    ESP_ELFSYM_EXPORT(process__switch_next),
    ESP_ELFSYM_EXPORT(process__switch_previous),
    ESP_ELFSYM_EXPORT(process__to_background),
    ESP_ELFSYM_EXPORT(process__to_foreground),
    ESP_ELFSYM_EXPORT(process__foreground),
    ESP_ELFSYM_EXPORT(process__signal),
    ESP_ELFSYM_EXPORT(process__terminate),
    ESP_ELFSYM_EXPORT(process__pause),
    ESP_ELFSYM_EXPORT(process__resume),
    ESP_ELFSYM_EXPORT(process__kill),
    ESP_ELFSYM_EXPORT(process__wait),
    ESP_ELFSYM_EXPORT(process__wait_status),
    ESP_ELFSYM_EXPORT(process__current_signal),
    ESP_ELFSYM_EXPORT(process__clear_signal),
    ESP_ELFSYM_EXPORT(process__snapshot),
    ESP_ELFSYM_EXPORT(process__list),

    /* Audio */
    ESP_ELFSYM_EXPORT(audio__tone),
    ESP_ELFSYM_EXPORT(audio__stream_sample_rate),
    ESP_ELFSYM_EXPORT(audio__stream_open),
    ESP_ELFSYM_EXPORT(audio__stream_writable_frames),
    ESP_ELFSYM_EXPORT(audio__stream_write),
    ESP_ELFSYM_EXPORT(audio__stream_close),

    /* Device state */
    ESP_ELFSYM_EXPORT(device__get_battery),
    ESP_ELFSYM_EXPORT(device__restart),
    ESP_ELFSYM_EXPORT(device__power_off),

    /* Wall clock */
    ESP_ELFSYM_EXPORT(clock__get_utc),
    ESP_ELFSYM_EXPORT(clock__get_local),
    ESP_ELFSYM_EXPORT(clock__set_local),
    ESP_ELFSYM_EXPORT(clock__sync_ntp),
    ESP_ELFSYM_EXPORT(clock__get_sync_status),
    ESP_ELFSYM_EXPORT(clock__get_ntp_server),

    /* Read-only application preferences. Protected values enforce config
     * permission in Core. */
    ESP_ELFSYM_EXPORT(config__get_color_primary),
    ESP_ELFSYM_EXPORT(config__get_color_secondary),
    ESP_ELFSYM_EXPORT(config__get_color_background),
    ESP_ELFSYM_EXPORT(config__get_color_surface),
    ESP_ELFSYM_EXPORT(config__get_color_text),
    ESP_ELFSYM_EXPORT(config__get_color_text_muted),
    ESP_ELFSYM_EXPORT(config__get_color_border),
    ESP_ELFSYM_EXPORT(config__get_color_success),
    ESP_ELFSYM_EXPORT(config__get_color_warning),
    ESP_ELFSYM_EXPORT(config__get_color_error),
    ESP_ELFSYM_EXPORT(config__get_time_clock24hr),

    /* AppRunner / loader */
    ESP_ELFSYM_EXPORT(app_runner__run),
    ESP_ELFSYM_EXPORT(app_runner__run_path),
    ESP_ELFSYM_EXPORT(app_runner__run_with_environment),
    ESP_ELFSYM_EXPORT(app_runner__run_path_with_environment),
    ESP_ELFSYM_EXPORT(ext_mem_loader__stage_path),
    ESP_ELFSYM_EXPORT(ext_mem_loader__adopt_image),
    ESP_ELFSYM_EXPORT(ext_mem_loader__release_image),
    ESP_ELFSYM_EXPORT(app_runner__parse_args),
    ESP_ELFSYM_EXPORT(app_runner__free_args),
    ESP_ELFSYM_EXPORT(app_runner__environment_requests_gui),
    ESP_ELFSYM_EXPORT(app_runner__spawn_loader_process),
    ESP_ELFSYM_EXPORT(app_runner__spawn_loader_process_owned),
    ESP_ELFSYM_EXPORT(app_runner__spawn_loader_process_owned_with_stop),
    ESP_ELFSYM_EXPORT(app_runner__run_command),
    ESP_ELFSYM_EXPORT(app_runner__icon_for_path),

    /* Runtime dependencies used by statically linked external interpreters. */
    ESP_ELFSYM_EXPORT(pthread_self),
    ESP_ELFSYM_EXPORT(pthread_mutex_init),
    ESP_ELFSYM_EXPORT(pthread_mutex_destroy),
    ESP_ELFSYM_EXPORT(pthread_mutex_lock),
    ESP_ELFSYM_EXPORT(pthread_mutex_unlock),
    ESP_ELFSYM_EXPORT(bsearch),
    ESP_ELFSYM_EXPORT(qsort),
    ESP_ELFSYM_EXPORT(__errno),
    ESP_ELFSYM_EXPORT(__ieee754_sqrtf),

    /* Argument parser */
    ESP_ELFSYM_EXPORT(ap_new_parser),
    ESP_ELFSYM_EXPORT(ap_free),
    ESP_ELFSYM_EXPORT(ap_set_helptext),
    ESP_ELFSYM_EXPORT(ap_get_helptext),
    ESP_ELFSYM_EXPORT(ap_set_version),
    ESP_ELFSYM_EXPORT(ap_get_version),
    ESP_ELFSYM_EXPORT(ap_parse),
    ESP_ELFSYM_EXPORT(ap_get_status),
    ESP_ELFSYM_EXPORT(ap_print_help),
    ESP_ELFSYM_EXPORT(ap_first_pos_arg_ends_option_parsing),
    ESP_ELFSYM_EXPORT(ap_all_args_as_pos_args),
    ESP_ELFSYM_EXPORT(ap_allow_extra_args),
    ESP_ELFSYM_EXPORT(ap_unknown_options_as_args),
    ESP_ELFSYM_EXPORT(ap_add_flag),
    ESP_ELFSYM_EXPORT(ap_add_str_opt),
    ESP_ELFSYM_EXPORT(ap_add_int_opt),
    ESP_ELFSYM_EXPORT(ap_add_dbl_opt),
    ESP_ELFSYM_EXPORT(ap_add_greedy_str_opt),
    ESP_ELFSYM_EXPORT(ap_set_opt_help),
    ESP_ELFSYM_EXPORT(ap_count),
    ESP_ELFSYM_EXPORT(ap_found),
    ESP_ELFSYM_EXPORT(ap_get_str_value),
    ESP_ELFSYM_EXPORT(ap_get_str_value_at_index),
    ESP_ELFSYM_EXPORT(ap_get_str_values),
    ESP_ELFSYM_EXPORT(ap_get_int_value),
    ESP_ELFSYM_EXPORT(ap_get_int_value_at_index),
    ESP_ELFSYM_EXPORT(ap_get_int_values),
    ESP_ELFSYM_EXPORT(ap_get_dbl_value),
    ESP_ELFSYM_EXPORT(ap_get_dbl_value_at_index),
    ESP_ELFSYM_EXPORT(ap_get_dbl_values),
    ESP_ELFSYM_EXPORT(ap_add_required_arg),
    ESP_ELFSYM_EXPORT(ap_add_optional_arg),
    ESP_ELFSYM_EXPORT(ap_get_arg),
    ESP_ELFSYM_EXPORT(ap_has_args),
    ESP_ELFSYM_EXPORT(ap_count_args),
    ESP_ELFSYM_EXPORT(ap_get_arg_at_index),
    ESP_ELFSYM_EXPORT(ap_get_args),
    ESP_ELFSYM_EXPORT(ap_get_args_as_ints),
    ESP_ELFSYM_EXPORT(ap_get_args_as_doubles),
    ESP_ELFSYM_EXPORT(ap_new_cmd),
    ESP_ELFSYM_EXPORT(ap_set_cmd_callback),
    ESP_ELFSYM_EXPORT(ap_found_cmd),
    ESP_ELFSYM_EXPORT(ap_get_cmd_name),
    ESP_ELFSYM_EXPORT(ap_get_cmd_parser),
    ESP_ELFSYM_EXPORT(ap_get_cmd_exit_code),
    ESP_ELFSYM_EXPORT(ap_enable_help_command),
    ESP_ELFSYM_EXPORT(ap_get_parent),
    ESP_ELFSYM_EXPORT(ap_print),
    ESP_ELFSYM_EXPORT(ap_had_memory_error),
    ESP_ELFSYM_EXPORT(ap_get_zeroth_root_arg),

    /* Process environment */
    ESP_ELFSYM_EXPORT(environment__global_get),
    ESP_ELFSYM_EXPORT(environment__global_set),
    ESP_ELFSYM_EXPORT(environment__global_unset),
    ESP_ELFSYM_EXPORT(environment__get),
    ESP_ELFSYM_EXPORT(environment__set),
    ESP_ELFSYM_EXPORT(environment__unset),
    ESP_ELFSYM_EXPORT(environment__count),
    ESP_ELFSYM_EXPORT(environment__get_at),

    /* Memory */
    ESP_ELFSYM_EXPORT(memory__malloc),
    ESP_ELFSYM_EXPORT(memory__calloc),
    ESP_ELFSYM_EXPORT(memory__realloc),
    ESP_ELFSYM_EXPORT(memory__free),
    ESP_ELFSYM_EXPORT(memory__external_malloc),
    ESP_ELFSYM_EXPORT(memory__external_malloc_writable),
    ESP_ELFSYM_EXPORT(memory__external_calloc),
    ESP_ELFSYM_EXPORT(memory__external_memcpy),
    ESP_ELFSYM_EXPORT(memory__external_memset),
    ESP_ELFSYM_EXPORT(memory__external_free),
    ESP_ELFSYM_EXPORT(memory__get_stats),
    ESP_ELFSYM_EXPORT(memory__get_layout),
    ESP_ELFSYM_EXPORT(memory__reclaim),
    {"malloc",  (const void *)&memory__malloc    },
    {"calloc",  (const void *)&memory__calloc    },
    {"realloc", (const void *)&memory__realloc   },
    {"free",    (const void *)&memory__free      },

    /* Permission (introspection only; protected APIs check internally) */
    ESP_ELFSYM_EXPORT(permission__check),
    ESP_ELFSYM_EXPORT(permission__from_name),
    ESP_ELFSYM_EXPORT(permission__name),

    /* Wi-Fi and HTTP client */
    ESP_ELFSYM_EXPORT(wifi__disconnect),
    ESP_ELFSYM_EXPORT(wifi__connect),
    ESP_ELFSYM_EXPORT(wifi__connect_known),
    ESP_ELFSYM_EXPORT(wifi__setup_ap),
    ESP_ELFSYM_EXPORT(wifi__scan),
    ESP_ELFSYM_EXPORT(wifi__scan_start),
    ESP_ELFSYM_EXPORT(wifi__scan_poll),
    ESP_ELFSYM_EXPORT(wifi__scan_cancel),
    ESP_ELFSYM_EXPORT(wifi__is_connected),
    ESP_ELFSYM_EXPORT(wifi__is_ap_running),
    ESP_ELFSYM_EXPORT(wifi__get_ssid),
    ESP_ELFSYM_EXPORT(wifi__get_ip),
    ESP_ELFSYM_EXPORT(wifi__get_mac),
    ESP_ELFSYM_EXPORT(http__request),
    ESP_ELFSYM_EXPORT(http__response_free),

    /* Input (read is foreground-only; inject requires input permission) */
    ESP_ELFSYM_EXPORT(input__read),
    ESP_ELFSYM_EXPORT(input__poll),
    ESP_ELFSYM_EXPORT(input__flush),
    ESP_ELFSYM_EXPORT(input__peek),
    ESP_ELFSYM_EXPORT(input__wait),
    ESP_ELFSYM_EXPORT(input__check),
    ESP_ELFSYM_EXPORT(input__inject),

    /* Bluetooth advertisement scan and Classic HID host */
    ESP_ELFSYM_EXPORT(bluetooth__scan_ble),
    ESP_ELFSYM_EXPORT(bluetooth__scan_start),
    ESP_ELFSYM_EXPORT(bluetooth__scan_poll),
    ESP_ELFSYM_EXPORT(bluetooth__scan_cancel),
    ESP_ELFSYM_EXPORT(bluetooth_hid__is_supported),
    ESP_ELFSYM_EXPORT(bluetooth_hid__scan),
    ESP_ELFSYM_EXPORT(bluetooth_hid__connect),
    ESP_ELFSYM_EXPORT(bluetooth_hid__disconnect),
    ESP_ELFSYM_EXPORT(bluetooth_hid__is_connected),
    ESP_ELFSYM_EXPORT(bluetooth_hid__connected_device),

    /* Infrared */
    ESP_ELFSYM_EXPORT(ir__transmit_raw),
    ESP_ELFSYM_EXPORT(ir__transmit_code),
    ESP_ELFSYM_EXPORT(ir__transmit),
    ESP_ELFSYM_EXPORT(ir__transmit_parsed),
    ESP_ELFSYM_EXPORT(ir__receive),
    ESP_ELFSYM_EXPORT(ir__transmit_file),
    ESP_ELFSYM_EXPORT(ir__transmit_record),
    ESP_ELFSYM_EXPORT(ir__tx_pin),
    ESP_ELFSYM_EXPORT(ir__rx_pin),

    /* NRF24 passive radio operations */
    ESP_ELFSYM_EXPORT(nrf24__probe),
    ESP_ELFSYM_EXPORT(nrf24__set_channel),
    ESP_ELFSYM_EXPORT(nrf24__get_channel),
    ESP_ELFSYM_EXPORT(nrf24__scan),
    ESP_ELFSYM_EXPORT(nrf24__get_pins),

    /* GPIO and serial buses */
    ESP_ELFSYM_EXPORT(gpio__configure),
    ESP_ELFSYM_EXPORT(gpio__read),
    ESP_ELFSYM_EXPORT(gpio__write),
    ESP_ELFSYM_EXPORT(i2c__open),
    ESP_ELFSYM_EXPORT(i2c__probe),
    ESP_ELFSYM_EXPORT(i2c__write),
    ESP_ELFSYM_EXPORT(i2c__read),
    ESP_ELFSYM_EXPORT(i2c__write_read),
    ESP_ELFSYM_EXPORT(i2c__close),
    ESP_ELFSYM_EXPORT(spi__open),
    ESP_ELFSYM_EXPORT(spi__transfer),
    ESP_ELFSYM_EXPORT(spi__close),

    /* Named-topic pub/sub (e.g. BRUCE_DEVICE_TOPIC_TOUCH) */
    ESP_ELFSYM_EXPORT(pubsub__publish),
    ESP_ELFSYM_EXPORT(pubsub__subscribe),
    ESP_ELFSYM_EXPORT(pubsub__unsubscribe),
    ESP_ELFSYM_EXPORT(pubsub__read),

    /* Display (layout management remains built-in-only) */
    ESP_ELFSYM_EXPORT(display__width),
    ESP_ELFSYM_EXPORT(display__height),
    ESP_ELFSYM_EXPORT(display__color565),
    ESP_ELFSYM_EXPORT(display__fill_screen),
    ESP_ELFSYM_EXPORT(display__clear),
    ESP_ELFSYM_EXPORT(display__set_text_color),
    ESP_ELFSYM_EXPORT(display__set_text_bg_color),
    ESP_ELFSYM_EXPORT(display__set_text_size),
    ESP_ELFSYM_EXPORT(display__set_cursor),
    ESP_ELFSYM_EXPORT(display__get_cursor),
    ESP_ELFSYM_EXPORT(display__get_font_metrics),
    ESP_ELFSYM_EXPORT(display__print),
    ESP_ELFSYM_EXPORT(display__println),
    ESP_ELFSYM_EXPORT(display__draw_string),
    ESP_ELFSYM_EXPORT(display__draw_centre_string),
    ESP_ELFSYM_EXPORT(display__draw_right_string),
    ESP_ELFSYM_EXPORT(display__draw_pixel),
    ESP_ELFSYM_EXPORT(display__draw_line),
    ESP_ELFSYM_EXPORT(display__draw_rect),
    ESP_ELFSYM_EXPORT(display__fill_rect),
    ESP_ELFSYM_EXPORT(display__draw_circle),
    ESP_ELFSYM_EXPORT(display__fill_circle),
    ESP_ELFSYM_EXPORT(display__draw_arc),
    ESP_ELFSYM_EXPORT(display__draw_triangle),
    ESP_ELFSYM_EXPORT(display__fill_triangle),
    ESP_ELFSYM_EXPORT(display__draw_round_rect),
    ESP_ELFSYM_EXPORT(display__fill_round_rect),
    ESP_ELFSYM_EXPORT(display__draw_bitmap),
    ESP_ELFSYM_EXPORT(display__draw_xbitmap),
    ESP_ELFSYM_EXPORT(display__draw_rgb_bitmap),
    ESP_ELFSYM_EXPORT(display__draw_bitmap_scaled),
    ESP_ELFSYM_EXPORT(display__set_rotation),
    ESP_ELFSYM_EXPORT(display__get_rotation),
    ESP_ELFSYM_EXPORT(display__invert_display),
    ESP_ELFSYM_EXPORT(display__set_brightness),
    ESP_ELFSYM_EXPORT(display__get_brightness),
    ESP_ELFSYM_EXPORT(display__display_on_off),
    ESP_ELFSYM_EXPORT(display__request_render_mode),
    ESP_ELFSYM_EXPORT(display__release_render_mode),
    ESP_ELFSYM_EXPORT(display__buffer_footprint),
    ESP_ELFSYM_EXPORT(display__begin_frame),
    ESP_ELFSYM_EXPORT(display__present),
    ESP_ELFSYM_EXPORT(display__screen_width),
    ESP_ELFSYM_EXPORT(display__screen_height),

    /* Overlays: a small always-on-top drawing surface any process may
     * create for its own menu/notification/HUD (see core_sdk/display.h). */
    ESP_ELFSYM_EXPORT(display__overlay_create),
    ESP_ELFSYM_EXPORT(display__overlay_destroy),
    ESP_ELFSYM_EXPORT(display__overlay_show),
    ESP_ELFSYM_EXPORT(display__overlay_hide),
    ESP_ELFSYM_EXPORT(display__overlay_move),
    ESP_ELFSYM_EXPORT(display__overlay_begin),
    ESP_ELFSYM_EXPORT(display__overlay_end),

    /* Built-in vector icons */
    ESP_ELFSYM_EXPORT(icon__get),

    /* Encoded images */
    ESP_ELFSYM_EXPORT(image__get_bitmap_from_memory),
    ESP_ELFSYM_EXPORT(image__get_bitmap_from_file),
    ESP_ELFSYM_EXPORT(image__bitmap_resize),
    ESP_ELFSYM_EXPORT(image__draw_path),
    ESP_ELFSYM_EXPORT(image__draw_bitmap),
    ESP_ELFSYM_EXPORT(image__bitmap_release),
    ESP_ELFSYM_EXPORT(image__is_supported_path),
    ESP_ELFSYM_EXPORT(image__gif_open),
    ESP_ELFSYM_EXPORT(image__gif_draw),
    ESP_ELFSYM_EXPORT(image__gif_increment),
    ESP_ELFSYM_EXPORT(image__gif_close),

    /* Unrestricted global UI services */
    ESP_ELFSYM_EXPORT(notification__push),
    ESP_ELFSYM_EXPORT(notification__dismiss),
    ESP_ELFSYM_EXPORT(status_icon__push),
    ESP_ELFSYM_EXPORT(status_icon__push_named),
    ESP_ELFSYM_EXPORT(status_icon__remove),
    ESP_ELFSYM_EXPORT(status_icon__list),
    ESP_ELFSYM_EXPORT(status_icon__get),

    /* Dialog */
    ESP_ELFSYM_EXPORT(dialog__message),
    ESP_ELFSYM_EXPORT(dialog__choice),
    ESP_ELFSYM_EXPORT(dialog__choice_launcher),
    ESP_ELFSYM_EXPORT(dialog__choice_ex),
    ESP_ELFSYM_EXPORT(dialog__pick_file),
    ESP_ELFSYM_EXPORT(dialog__text_input),
    ESP_ELFSYM_EXPORT(dialog__hex_input),
    ESP_ELFSYM_EXPORT(dialog__number_input),
    ESP_ELFSYM_EXPORT(dialog__create_text_viewer),
    ESP_ELFSYM_EXPORT(dialog__viewer_set_text),
    ESP_ELFSYM_EXPORT(dialog__viewer_set_text_size),
    ESP_ELFSYM_EXPORT(dialog__viewer_scroll),
    ESP_ELFSYM_EXPORT(dialog__viewer_close),

    /* Manifest inspection */
    ESP_ELFSYM_EXPORT(manifest__parse),
    ESP_ELFSYM_EXPORT(manifest__inspect_path),
    ESP_ELFSYM_EXPORT(manifest__inspect_elf),
    ESP_ELFSYM_EXPORT(manifest__inspect_javascript),
    ESP_ELFSYM_EXPORT(manifest__inspect_wasm),

    /* Storage */
    ESP_ELFSYM_EXPORT(storage__open),
    ESP_ELFSYM_EXPORT(storage__read),
    ESP_ELFSYM_EXPORT(storage__write),
    ESP_ELFSYM_EXPORT(storage__seek),
    ESP_ELFSYM_EXPORT(storage__close),
    ESP_ELFSYM_EXPORT(storage__list),
    ESP_ELFSYM_EXPORT(storage__mkdir),
    ESP_ELFSYM_EXPORT(storage__exists),
    ESP_ELFSYM_EXPORT(storage__remove),
    ESP_ELFSYM_EXPORT(storage__rename),
    ESP_ELFSYM_EXPORT(storage__copy),
    ESP_ELFSYM_EXPORT(storage__get_usage),

    /* Block devices */
    ESP_ELFSYM_EXPORT(disk__list),
    ESP_ELFSYM_EXPORT(disk__mount),
    ESP_ELFSYM_EXPORT(disk__unmount),

    /* TCP and console streams */
    ESP_ELFSYM_EXPORT(tcp__connect),
    ESP_ELFSYM_EXPORT(tcp__listen),
    ESP_ELFSYM_EXPORT(tcp__accept),
    ESP_ELFSYM_EXPORT(tcp__read),
    ESP_ELFSYM_EXPORT(tcp__write),
    ESP_ELFSYM_EXPORT(tcp__close),
    ESP_ELFSYM_EXPORT(ssh__connect),
    ESP_ELFSYM_EXPORT(ssh__host_key_sha256),
    ESP_ELFSYM_EXPORT(ssh__verify_host_key_sha256),
    ESP_ELFSYM_EXPORT(ssh__authenticate_password),
    ESP_ELFSYM_EXPORT(ssh__generate_keypair_ex),
    ESP_ELFSYM_EXPORT(ssh__authenticate_key),
    ESP_ELFSYM_EXPORT(ssh__open_shell),
    ESP_ELFSYM_EXPORT(ssh__resize_pty),
    ESP_ELFSYM_EXPORT(ssh__read),
    ESP_ELFSYM_EXPORT(ssh__write),
    ESP_ELFSYM_EXPORT(ssh__close),
    ESP_ELFSYM_EXPORT(stdio__read),
    ESP_ELFSYM_EXPORT(stdio__read_line),
    ESP_ELFSYM_EXPORT(stdio__write),
    ESP_ELFSYM_EXPORT(stdio__printf),
    ESP_ELFSYM_EXPORT(stdio__vprintf),
    ESP_ELFSYM_EXPORT(stdio__session_create),
    ESP_ELFSYM_EXPORT(stdio__session_close),
    ESP_ELFSYM_EXPORT(stdio__session_route_children),
    ESP_ELFSYM_EXPORT(stdio__session_write_input),
    ESP_ELFSYM_EXPORT(stdio__session_read_output),
    ESP_ELFSYM_EXPORT(tty__isatty),
    ESP_ELFSYM_EXPORT(tty__get_size),
    ESP_ELFSYM_EXPORT(tty__set_size),
    ESP_ELFSYM_EXPORT(tty__get_mode),
    ESP_ELFSYM_EXPORT(tty__set_mode),

    /* Standard C library subset. Console and heap calls are routed through
     * process-aware Bruce SDK functions rather than firmware libc. */
    {"printf",  (const void *)&stdio__printf     },
    {"vprintf", (const void *)&stdio__vprintf    },
    {"puts",    (const void *)&bruce_elf__puts   },
    {"putchar", (const void *)&bruce_elf__putchar},

    /* FILE*-based stdio, backed by storage__open/read/write/seek/close --
     * see the bruce_elf_file_t doc comment above bruce_elf__fopen(). */
    {"stdin",    (const void *)&bruce_elf__stdin_ptr },
    {"stdout",   (const void *)&bruce_elf__stdout_ptr},
    {"stderr",   (const void *)&bruce_elf__stderr_ptr},
    {"fopen",    (const void *)&bruce_elf__fopen    },
    {"fclose",   (const void *)&bruce_elf__fclose   },
    {"fread",    (const void *)&bruce_elf__fread    },
    {"fwrite",   (const void *)&bruce_elf__fwrite   },
    {"fseek",    (const void *)&bruce_elf__fseek    },
    {"ftell",    (const void *)&bruce_elf__ftell    },
    {"rewind",   (const void *)&bruce_elf__rewind   },
    {"fflush",   (const void *)&bruce_elf__fflush   },
    {"fgetc",    (const void *)&bruce_elf__fgetc    },
    {"fputc",    (const void *)&bruce_elf__fputc    },
    {"getc",     (const void *)&bruce_elf__fgetc    },
    {"putc",     (const void *)&bruce_elf__fputc    },
    {"fgets",    (const void *)&bruce_elf__fgets    },
    {"fputs",    (const void *)&bruce_elf__fputs    },
    {"feof",     (const void *)&bruce_elf__feof     },
    {"ferror",   (const void *)&bruce_elf__ferror   },
    {"clearerr", (const void *)&bruce_elf__clearerr },
    {"remove",   (const void *)&bruce_elf__remove   },
    {"rename",   (const void *)&bruce_elf__rename   },
    {"fprintf",  (const void *)&bruce_elf__fprintf  },
    {"vfprintf", (const void *)&bruce_elf__vfprintf },
    {"setvbuf",  (const void *)&bruce_elf__setvbuf  },
    {"getenv",   (const void *)&bruce_elf__getenv   },
    {"setenv",   (const void *)&bruce_elf__setenv   },
    {"unsetenv", (const void *)&bruce_elf__unsetenv },
    {"strdup",   (const void *)&bruce_elf__strdup   },
    {"strndup",  (const void *)&bruce_elf__strndup  },
    {"__assert_func", (const void *)&bruce_elf__assert_func},
    {"__assert",      (const void *)&bruce_elf__assert     },
    ESP_ELFSYM_EXPORT(time),
    ESP_ELFSYM_EXPORT(gmtime),
    ESP_ELFSYM_EXPORT(gmtime_r),
    {"localtime",  (const void *)&bruce_elf__localtime  },
    {"localtime_r",(const void *)&bruce_elf__localtime_r},
    {"mktime",     (const void *)&bruce_elf__mktime    },
    {"clock",      (const void *)&bruce_elf__clock     },
    ESP_ELFSYM_EXPORT(difftime),
    ESP_ELFSYM_EXPORT(strftime),
    ESP_ELFSYM_EXPORT(asctime),
    ESP_ELFSYM_EXPORT(asctime_r),
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(sprintf),
    ESP_ELFSYM_EXPORT(vsnprintf),
    ESP_ELFSYM_EXPORT(sscanf),
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strcat),
    ESP_ELFSYM_EXPORT(strncat),
    ESP_ELFSYM_EXPORT(strchr),
    ESP_ELFSYM_EXPORT(strrchr),
    ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(memchr),
    ESP_ELFSYM_EXPORT(strtok),
    ESP_ELFSYM_EXPORT(strtok_r),
    ESP_ELFSYM_EXPORT(strspn),
    ESP_ELFSYM_EXPORT(strcspn),
    ESP_ELFSYM_EXPORT(strpbrk),
    ESP_ELFSYM_EXPORT(strcasecmp),
    ESP_ELFSYM_EXPORT(strncasecmp),
    ESP_ELFSYM_EXPORT(strtol),
    ESP_ELFSYM_EXPORT(strtoll),
    ESP_ELFSYM_EXPORT(strtoul),
    ESP_ELFSYM_EXPORT(strtoull),
    ESP_ELFSYM_EXPORT(strtod),
    ESP_ELFSYM_EXPORT(strtof),
    ESP_ELFSYM_EXPORT(atoi),
    ESP_ELFSYM_EXPORT(atol),
    ESP_ELFSYM_EXPORT(atoll),
    ESP_ELFSYM_EXPORT(atof),
    ESP_ELFSYM_EXPORT(abs),
    ESP_ELFSYM_EXPORT(labs),
    ESP_ELFSYM_EXPORT(llabs),

    /* GCC runtime helpers used by freestanding ELF code. */
    ESP_ELFSYM_EXPORT(__eqdf2),
    ESP_ELFSYM_EXPORT(__adddf3),
    ESP_ELFSYM_EXPORT(__divdi3),
    ESP_ELFSYM_EXPORT(__divsf3),
    ESP_ELFSYM_EXPORT(__addsf3),
    ESP_ELFSYM_EXPORT(__subsf3),
    ESP_ELFSYM_EXPORT(__mulsf3),
    ESP_ELFSYM_EXPORT(__floatsisf),
    ESP_ELFSYM_EXPORT(__fixsfsi),
    ESP_ELFSYM_EXPORT(__fixunssfsi),
    ESP_ELFSYM_EXPORT(__gesf2),
    ESP_ELFSYM_EXPORT(__ltsf2),
    ESP_ELFSYM_EXPORT(__fixdfdi),
    ESP_ELFSYM_EXPORT(__floatsidf),
    ESP_ELFSYM_EXPORT(__floatdisf),
    ESP_ELFSYM_EXPORT(__floatdidf),
    ESP_ELFSYM_EXPORT(__floatundidf),
    ESP_ELFSYM_EXPORT(__floatundisf),
    ESP_ELFSYM_EXPORT(__floatunsidf),
    ESP_ELFSYM_EXPORT(__extendsfdf2),
    ESP_ELFSYM_EXPORT(__fixdfsi),
    ESP_ELFSYM_EXPORT(__fixunsdfsi),
    ESP_ELFSYM_EXPORT(__fixunsdfdi),
    ESP_ELFSYM_EXPORT(__fixsfdi),
    ESP_ELFSYM_EXPORT(__fixunssfdi),
    ESP_ELFSYM_EXPORT(__gedf2),
    ESP_ELFSYM_EXPORT(__gtdf2),
    ESP_ELFSYM_EXPORT(__ledf2),
    ESP_ELFSYM_EXPORT(__ltdf2),
    ESP_ELFSYM_EXPORT(__moddi3),
    ESP_ELFSYM_EXPORT(__nedf2),
    ESP_ELFSYM_EXPORT(__unorddf2),
    ESP_ELFSYM_EXPORT(__umoddi3),
    ESP_ELFSYM_EXPORT(__divdf3),
    ESP_ELFSYM_EXPORT(__muldf3),
    ESP_ELFSYM_EXPORT(__subdf3),
    ESP_ELFSYM_EXPORT(__truncdfsf2),
    ESP_ELFSYM_EXPORT(__udivdi3),

    /* libm. Real implementations already exist in the firmware's own libm;
     * this just exposes them to ELF apps that do floating-point math
     * (e.g. native_apps/examples/game3d's 3D renderer). */
    ESP_ELFSYM_EXPORT(sin),
    ESP_ELFSYM_EXPORT(cos),
    ESP_ELFSYM_EXPORT(tan),
    ESP_ELFSYM_EXPORT(sinf),
    ESP_ELFSYM_EXPORT(cosf),
    ESP_ELFSYM_EXPORT(tanf),
    ESP_ELFSYM_EXPORT(sqrt),
    ESP_ELFSYM_EXPORT(sqrtf),
    ESP_ELFSYM_EXPORT(atan2),
    ESP_ELFSYM_EXPORT(atan2f),
    ESP_ELFSYM_EXPORT(fabsf),
    ESP_ELFSYM_EXPORT(fabs),
    ESP_ELFSYM_EXPORT(floor),
    ESP_ELFSYM_EXPORT(floorf),
    ESP_ELFSYM_EXPORT(ceil),
    ESP_ELFSYM_EXPORT(ceilf),
    ESP_ELFSYM_EXPORT(fmodf),
    ESP_ELFSYM_EXPORT(pow),
    ESP_ELFSYM_EXPORT(powf),
    ESP_ELFSYM_EXPORT(exp),
    ESP_ELFSYM_EXPORT(expf),
    ESP_ELFSYM_EXPORT(log),
    ESP_ELFSYM_EXPORT(logf),
    ESP_ELFSYM_EXPORT(log10),
    ESP_ELFSYM_EXPORT(log10f),
    ESP_ELFSYM_EXPORT(round),
    ESP_ELFSYM_EXPORT(roundf),
    ESP_ELFSYM_EXPORT(trunc),
    ESP_ELFSYM_EXPORT(truncf),
    ESP_ELFSYM_EXPORT(hypot),
    ESP_ELFSYM_EXPORT(hypotf),
    ESP_ELFSYM_EXPORT(asin),
    ESP_ELFSYM_EXPORT(asinf),
    ESP_ELFSYM_EXPORT(acos),
    ESP_ELFSYM_EXPORT(acosf),
    ESP_ELFSYM_EXPORT(frexp),
    ESP_ELFSYM_EXPORT(frexpf),
    ESP_ELFSYM_EXPORT(ldexp),
    ESP_ELFSYM_EXPORT(ldexpf),

    /* Pure, stateless-to-us runtime helpers with no Bruce-specific behavior
     * -- direct exports of the firmware's own real symbols, same rationale
     * as the libm block above. setjmp/longjmp are plain architecture asm
     * stubs (not OS-dependent), and rand/srand's internal state is real
     * libc state shared with the firmware itself -- not something that
     * needs per-process accounting the way malloc/free do. */
    ESP_ELFSYM_EXPORT(setjmp),
    ESP_ELFSYM_EXPORT(longjmp),
    ESP_ELFSYM_EXPORT(rand),
    ESP_ELFSYM_EXPORT(srand),

    /* ctype.h. This project's actual toolchain library is picolibc (checked
     * via `xtensa-esp32s3-elf-gcc -H`, not assumed from the toolchain's
     * "xtensa-esp-elf" directory name, which is really just the target
     * triple). picolibc's ctype.h has two implementations selected by
     * _PICOLIBC_CTYPE_SMALL: a single shared lookup-table symbol (like
     * newlib's _ctype_) when unset, or -- what this build actually gets,
     * since -Os (in this project's own COMPILE_OPTIONS) defines
     * __OPTIMIZE_SIZE__ which picolibc's ctype.h checks directly -- a set
     * of small `extern inline` functions with no shared table at all, each
     * with its own real out-of-line definition for exactly this kind of
     * address-taking use. Exporting each by name (verified compilable and
     * linkable, not assumed) is what actually works here; the single-table
     * shortcut this comment originally assumed does not apply to this
     * build's flags. toupper/tolower are these same real functions too, not
     * macros, on this library. */
    ESP_ELFSYM_EXPORT(isalnum),
    ESP_ELFSYM_EXPORT(isalpha),
    ESP_ELFSYM_EXPORT(iscntrl),
    ESP_ELFSYM_EXPORT(isdigit),
    ESP_ELFSYM_EXPORT(isgraph),
    ESP_ELFSYM_EXPORT(islower),
    ESP_ELFSYM_EXPORT(isprint),
    ESP_ELFSYM_EXPORT(ispunct),
    ESP_ELFSYM_EXPORT(isspace),
    ESP_ELFSYM_EXPORT(isupper),
    ESP_ELFSYM_EXPORT(isxdigit),
    ESP_ELFSYM_EXPORT(tolower),
    ESP_ELFSYM_EXPORT(toupper),

    /* C++ freestanding new/delete + pure-virtual trap (see the C++ ABI
     * comment block above). Mangled names, not ESP_ELFSYM_EXPORT: these are
     * C++ operators, not C symbols the preprocessor can name directly. */
    {"_Znwj",                               (const void *)&bruce_elf__operator_new         }, /* operator new(size_t) */
    {"_Znaj",                               (const void *)&bruce_elf__operator_new         }, /* operator new[](size_t) */
    {"_ZdlPv",                              (const void *)&bruce_elf__operator_delete      }, /* operator delete(void*) */
    {"_ZdaPv",                              (const void *)&bruce_elf__operator_delete      }, /* operator delete[](void*) */
    {"_ZdlPvj",                             (const void *)&bruce_elf__operator_delete_sized}, /* operator delete(void*, size_t) */
    {"_ZdaPvj",                             (const void *)&bruce_elf__operator_delete_sized}, /* operator delete[](void*, size_t) */
    {"__cxa_pure_virtual",                  (const void *)&bruce_elf__cxa_pure_virtual     },
    {"__cxa_atexit",                        (const void *)&bruce_elf__cxa_atexit           },
    {"_ZnwjRKSt9nothrow_t",
                       (const void *)&bruce_elf__operator_new_nothrow                                        }, /* operator new(size_t, const std::nothrow_t&) */
    {"_ZSt17__throw_bad_allocv",            (const void *)&bruce_elf__throw_bad_alloc      }, /* std::__throw_bad_alloc() */
    {"_ZSt20__throw_length_errorPKc",
                       (const void *)&bruce_elf__throw_length_error                                          }, /* std::__throw_length_error(const char*) */
    {"_ZSt28__throw_bad_array_new_lengthv",
                       (const void *)&bruce_elf__throw_bad_array_new_length                                  }, /* std::__throw_bad_array_new_length() */

    ESP_ELFSYM_END,
};
