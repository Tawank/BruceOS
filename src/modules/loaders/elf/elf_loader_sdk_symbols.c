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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "elf_loader_internal.h"
#include "esp_elf.h" // IWYU pragma: export

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "core/process/process.h"
#include "core_sdk/app_config.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/archive.h"
#include "core_sdk/args.h"
#include "core_sdk/audio.h"
#include "core_sdk/base64.h"
#include "core_sdk/bluetooth.h"
#include "core_sdk/bluetooth_hid.h"
#include "core_sdk/clipboard.h"
#include "core_sdk/clock.h"
#include "core_sdk/compress.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/dialog.h"
#include "core_sdk/disk.h"
#include "core_sdk/display.h"
#include "core_sdk/environment.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/filetype.h"
#include "core_sdk/gpio.h"
#include "core_sdk/hash.h"
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
#include "core_sdk/partition_manager.h"
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
#include "core_sdk/udp.h"
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
/* This build's own <ctype.h> (see the isalpha/isdigit/etc. block below)
 * uses picolibc's "small" ctype variant, where is*()/toupper()/tolower()
 * are real out-of-line functions and the header never declares this raw
 * classification table at all (it only exists under picolibc's other,
 * larger "table" ctype variant, gated by _PICOLIBC_CTYPE_SMALL -- see
 * that header's own #else branch). The table itself, however, is still
 * a real, always-present symbol in the prebuilt libc archive: some
 * numeric-parsing internals there (e.g. strtoul(), which vi's
 * bb_strtou() calls -- see native_apps/examples/vi/main/busybox_shim.c)
 * reference it directly regardless of which ctype variant the *header*
 * exposes to our own source. Declared by hand for the same reason
 * __errno/__muldi3/etc. above are: the linker resolves it from the
 * archive on its own once a symbol of this name is referenced, no
 * header needed. */
extern const char _ctype_b[];
/* Xtensa has no 64x64 hardware multiply either, so GCC lowers a plain
 * `int64_t * int64_t` (e.g. Doom's FixedMul: `((int64_t)a * (int64_t)b) >>
 * FRACBITS`, its single hottest-path arithmetic op) to this libgcc call,
 * same rationale as the other __*di3 entries above. */
extern long long __muldi3(long long left, long long right);

static int bruce_elf__puts(const char *text) {
    if (text == NULL || stdio__write(text, strlen(text)) != BRUCE_OK) return EOF;
    return stdio__write("\n", 1) == BRUCE_OK ? 0 : EOF;
}

static int bruce_elf__putchar(int character) {
    unsigned char byte = (unsigned char)character;
    return stdio__write(&byte, 1) == BRUCE_OK ? byte : EOF;
}

/* BruceOS has no multi-user concept -- storage__'s stat()/fstat() likewise
 * always report st_uid 0 (bruce_elf__stat_fill()) -- so this just reports
 * the one uid every file already appears to be owned by, matching real
 * getuid()'s never-fails contract. Exists for BusyBox vi's ".exrc must
 * belong to the invoking user" ownership check (editors/vi.c), which would
 * otherwise be an unresolved symbol (a real libc always provides getuid(),
 * so it's never gated behind a feature flag upstream). */
static uid_t bruce_elf__getuid(void) {
    return 0;
}

/* A GNU libc extension (strchr(), but returns a pointer to the trailing NUL
 * instead of NULL when the character isn't found) this toolchain's picolibc
 * doesn't provide -- pure string-scanning with no OS dependency, so it's
 * implemented directly rather than routed through any core_sdk/ call. */
static char *bruce_elf__strchrnul(const char *text, int character) {
    while (*text != '\0' && *text != (char)character) text++;
    return (char *)text;
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
    /* BSD-socket-shaped fds -- see the "BSD sockets" section below, well
     * after the rest of this POSIX-fd family, for what each of these means
     * and why a socket needs more than one kind to represent it. */
    BRUCE_ELF_FILE_SOCKET_TCP_PENDING,
    BRUCE_ELF_FILE_SOCKET_TCP_STREAM,
    BRUCE_ELF_FILE_SOCKET_TCP_LISTENER,
    BRUCE_ELF_FILE_SOCKET_UDP,
} bruce_elf_file_kind_t;

typedef struct {
    bruce_elf_file_kind_t kind;
    union {
        bruce_file_id_t file; /* kind == BRUCE_ELF_FILE_STORAGE */
        bruce_tcp_id_t tcp;   /* kind == ..._SOCKET_TCP_STREAM or ..._TCP_LISTENER */
        bruce_udp_id_t udp;   /* kind == ..._SOCKET_UDP */
    };
    bool eof;
    bool error;
    /* kind == ..._SOCKET_TCP_PENDING only: the port bind() recorded, applied
     * by a later listen() (there is no real tcp__ handle yet to bind). */
    uint16_t pending_port;
    /* kind == ..._SOCKET_UDP only: the peer connect() latched, if any. */
    bool udp_connected;
    char udp_peer_host[BRUCE_UDP_HOST_MAX];
    uint16_t udp_peer_port;
} bruce_elf_file_t;

static bool bruce_elf__file_kind_is_socket(bruce_elf_file_kind_t kind) {
    switch (kind) {
        case BRUCE_ELF_FILE_SOCKET_TCP_PENDING:
        case BRUCE_ELF_FILE_SOCKET_TCP_STREAM:
        case BRUCE_ELF_FILE_SOCKET_TCP_LISTENER:
        case BRUCE_ELF_FILE_SOCKET_UDP:
            return true;
        default:
            return false;
    }
}

/* Defined in the "BSD sockets" section below; forward-declared here so
 * bruce_elf__close()/read()/write() (part of the existing POSIX-fd family,
 * defined next) can dispatch a socket-kind fd into it without that whole
 * section needing to sit before them. */
static int bruce_elf__socket_close(bruce_elf_file_t *box);
static ssize_t bruce_elf__socket_read(bruce_elf_file_t *box, void *buffer, size_t count);
static ssize_t bruce_elf__socket_write(bruce_elf_file_t *box, const void *buffer, size_t count);

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
        case BRUCE_ERR_BUSY: *__errno() = EADDRINUSE; break; /* bind()'s own most common failure */
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

/* perror(), routed through bruce_elf__fprintf(bruce_elf__stderr_ptr, ...)
 * rather than exported as real picolibc's own perror() the way strerror()
 * below is: real perror() writes to picolibc's own internal `stderr` global
 * (whatever real FILE the firmware's own libc considers stderr, e.g. its
 * UART console), not the value this table hands the ELF app for "stderr"
 * (bruce_elf__stderr_ptr, a console sentinel box -- see the FILE* doc
 * comment above) -- calling straight into the real function would silently
 * bypass this process's own stdio__ routing instead of using it, same
 * reasoning as bruce_elf__puts() over a hypothetical direct fputs(stdout)
 * export. strerror() itself is a pure errnum->string lookup with no I/O, so
 * it's exported unadapted below. Not declared `static`, like the rest of
 * this block, so modules/selftest can call it directly. */
void bruce_elf__perror(const char *prefix) {
    const char *message = strerror(*__errno());
    if (prefix != NULL && prefix[0] != '\0') {
        bruce_elf__fprintf(bruce_elf__stderr_ptr, "%s: %s\n", prefix, message);
    } else {
        bruce_elf__fprintf(bruce_elf__stderr_ptr, "%s\n", message);
    }
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
 * stdlib.h process termination: exit()/_exit()/abort(). Unlike a real
 * process, a sandboxed ELF app has no OS process boundary underneath it for
 * these to unwind through -- normally exit()/abort() are the kernel tearing
 * a process down out from under it, no matter how deep its call stack is.
 * elf_loader_app.c's elf_loader__entry() sets up the closest equivalent
 * available here: a setjmp() taken right before handing control to the
 * loaded ELF's own code, armed via process_registry__set_sandbox_exit_target()
 * (core/process/process.h) so it can be found again from here with no
 * parameter to thread through (exit()/abort() are resolved as ordinary libc
 * symbols; their signature has no room for one). bruce_elf__exit_common()
 * below stashes the exit status where elf_loader__entry() will read it back
 * and longjmp()s there, unwinding out of however many native call frames the
 * loaded ELF's own code was nested in -- see elf_loader_internal.h for the
 * shared bruce_elf_exit_context_t and elf_loader__entry()'s own comment for
 * the setjmp() side of this pair.
 *
 * If there is no armed target -- should not happen while resolved through
 * this table (it only exists while elf_loader__entry() is running one), but
 * checked rather than blindly dereferenced -- both fall back to the same
 * print-and-park behavior bruce_elf__assert_func() and __cxa_pure_virtual
 * above use for their own "nowhere sane to return to" case.
 *
 * abort() additionally reports BRUCE_ELF_ABORT_EXIT_CODE (128 + SIGABRT,
 * see elf_loader_internal.h) instead of a caller-chosen status, and logs
 * before unwinding, matching what a real abort() prints before the SIGABRT
 * it raises takes the process down -- there is no signal delivery here for
 * it to actually raise, so the longjmp() is this abort()'s entire "default
 * action", not a caught one.
 *
 * Neither one runs the loaded ELF's own atexit()-registered destructors:
 * bruce_elf__cxa_atexit() above already documents that this loader has no
 * real C runtime driving exit() to call them; this longjmp() doesn't change
 * that, it's still just an unwind, not a runtime. _exit() is the exact same
 * function as exit() here for the same reason -- the distinction (skip
 * atexit() handlers) is meaningless when neither one ever ran them anyway. */
static _Noreturn void bruce_elf__exit_common(int status, const char *name) {
    bruce_elf_exit_context_t *exit_ctx = (bruce_elf_exit_context_t *)process_registry__sandbox_exit_target();
    if (exit_ctx == NULL) {
        stdio__printf("bruce: %s() called with no sandboxed app running\n", name);
        for (;;) { runtime__delay(1000); }
    }
    exit_ctx->exit_code = status;
    longjmp(exit_ctx->target, 1);
}

_Noreturn void bruce_elf__exit(int status) { bruce_elf__exit_common(status, "exit"); }

_Noreturn void bruce_elf__abort(void) {
    stdio__printf("bruce: Aborted\n");
    bruce_elf__exit_common(BRUCE_ELF_ABORT_EXIT_CODE, "abort");
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
 * POSIX low-level file I/O (open/close/read/write/lseek), metadata
 * (stat/fstat), and directory listing (opendir/readdir/closedir/rewinddir),
 * all layered on the exact same storage__* calls and bruce_elf_file_t box
 * the FILE*-based stdio family above already uses -- this is the
 * int-fd-shaped surface next to that pointer-shaped one, not a second
 * implementation. mkdir/access/unlink are thin storage__mkdir/exists/remove
 * wrappers (unlink is literally bruce_elf__remove under another name -- see
 * the table entries below).
 *
 * A fixed-size fd table maps small integers to the same boxes fopen() hands
 * out as FILE*, with fd 0/1/2 permanently bound to the stdin/stdout/stderr
 * console sentinels above (g_bruce_elf_console_in/out) -- so `write(1, ...)`
 * and `read(0, ...)` work too, not just their FILE* equivalents. Closing
 * fd 0/1/2 is a documented no-op, same as fclose() on stdin/stdout/stderr:
 * bruce_elf__fclose() already refuses to free a non-STORAGE box, so
 * bruce_elf__close() reusing it is correct as-is; the table slot is simply
 * never cleared for those three reserved descriptors.
 *
 * stat()/fstat() populate only the fields BruceOS's storage layer actually
 * tracks (size and file-vs-directory) -- st_dev/st_ino/st_uid/st_gid/st_rdev
 * and all three timestamps are always zero, a real, documented gap rather
 * than fabricated data. "is this path a directory" reuses this codebase's
 * own standing idiom (see storage__copy()'s comment in core/storage/storage.c):
 * storage__list(path, NULL, 0, &count) succeeding means a directory, and
 * BRUCE_ERR_IO specifically means "exists but isn't one".
 *
 * opendir() takes a two-pass snapshot (size the listing, then fetch it in
 * full into one malloc'd array) rather than exposing any live cursor into
 * storage__list() -- there isn't one to expose (out_count is always "how
 * many total", not an offset/cursor API) -- so a directory that changes
 * between those two calls, or between opendir() and a later readdir(), is a
 * known, documented snapshot-vs-live race, not something this shim can
 * avoid without storage__* itself growing pagination. */

#define BRUCE_ELF_FD_MAX 32
#define BRUCE_ELF_FD_RESERVED 3 /* 0/1/2 = stdin/stdout/stderr, pre-bound below */

static bruce_elf_file_t *s_bruce_elf_fd_table[BRUCE_ELF_FD_MAX] = {
    [0] = &g_bruce_elf_console_in,
    [1] = &g_bruce_elf_console_out,
    [2] = &g_bruce_elf_console_out,
};

static int bruce_elf__fd_alloc(bruce_elf_file_t *box) {
    for (int fd = BRUCE_ELF_FD_RESERVED; fd < BRUCE_ELF_FD_MAX; ++fd) {
        if (s_bruce_elf_fd_table[fd] == NULL) {
            s_bruce_elf_fd_table[fd] = box;
            return fd;
        }
    }
    return -1;
}

static bruce_elf_file_t *bruce_elf__fd_lookup(int fd) {
    if (fd < 0 || fd >= BRUCE_ELF_FD_MAX) return NULL;
    return s_bruce_elf_fd_table[fd];
}

static uint32_t bruce_elf__translate_open_flags(int flags) {
    uint32_t result = 0;
    int access_mode = flags & O_ACCMODE;
    if (access_mode == O_WRONLY) {
        result |= BRUCE_STORAGE_OPEN_WRITE;
    } else if (access_mode == O_RDWR) {
        result |= BRUCE_STORAGE_OPEN_READ | BRUCE_STORAGE_OPEN_WRITE;
    } else {
        result |= BRUCE_STORAGE_OPEN_READ;
    }
    if (flags & O_APPEND) result |= BRUCE_STORAGE_OPEN_APPEND;
    if (flags & O_CREAT) result |= BRUCE_STORAGE_OPEN_CREATE;
    if (flags & O_TRUNC) result |= BRUCE_STORAGE_OPEN_TRUNCATE;
    return result;
}

/* The POSIX mode_t third argument (only meaningful with O_CREAT) is
 * silently ignored via `...` rather than read -- BruceOS storage has no
 * permission-bit concept for files, same rationale as bruce_elf__mkdir()
 * below. */
int bruce_elf__open(const char *path, int flags, ...) {
    if (path == NULL) {
        *__errno() = EINVAL;
        return -1;
    }
    bruce_elf_file_t *box = memory__malloc(sizeof(*box));
    if (box == NULL) {
        *__errno() = ENOMEM;
        return -1;
    }
    box->kind = BRUCE_ELF_FILE_STORAGE;
    box->eof = false;
    box->error = false;
    bruce_result_t result = storage__open(path, bruce_elf__translate_open_flags(flags), &box->file);
    if (result != BRUCE_OK) {
        memory__free(box);
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    int fd = bruce_elf__fd_alloc(box);
    if (fd < 0) {
        storage__close(box->file);
        memory__free(box);
        *__errno() = EMFILE;
        return -1;
    }
    return fd;
}

int bruce_elf__close(int fd) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    int result = bruce_elf__file_kind_is_socket(box->kind) ? bruce_elf__socket_close(box)
                                                             : bruce_elf__fclose((FILE *)box);
    if (fd >= BRUCE_ELF_FD_RESERVED) s_bruce_elf_fd_table[fd] = NULL;
    return result == 0 ? 0 : -1;
}

ssize_t bruce_elf__read(int fd, void *buffer, size_t count) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (bruce_elf__file_kind_is_socket(box->kind)) return bruce_elf__socket_read(box, buffer, count);
    size_t read_count = bruce_elf__fread(buffer, 1, count, (FILE *)box);
    if (read_count == 0 && count != 0 && box->error) {
        *__errno() = EIO;
        return -1;
    }
    return (ssize_t)read_count;
}

ssize_t bruce_elf__write(int fd, const void *buffer, size_t count) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (bruce_elf__file_kind_is_socket(box->kind)) return bruce_elf__socket_write(box, buffer, count);
    size_t written = bruce_elf__fwrite(buffer, 1, count, (FILE *)box);
    if (written == 0 && count != 0 && box->error) {
        *__errno() = EIO;
        return -1;
    }
    return (ssize_t)written;
}

off_t bruce_elf__lseek(int fd, off_t offset, int whence) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return (off_t)-1;
    }
    if (bruce_elf__fseek((FILE *)box, (long)offset, whence) != 0) return (off_t)-1;
    return (off_t)bruce_elf__ftell((FILE *)box);
}

/* Needed by vi's file_write() (BusyBox's vi.c, editors/vi.c) to shrink a
 * saved file back down after an in-place overwrite -- it deliberately
 * opens without O_TRUNC and ftruncate()s afterwards instead (see its own
 * comment: reduces data lost on power fail versus truncating up front).
 * Backed by storage__truncate(), a thin wrapper over the real POSIX
 * ftruncate() storage.c already has on its internal fd (core/storage/storage.c). */
int bruce_elf__ftruncate(int fd, off_t length) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (box->kind != BRUCE_ELF_FILE_STORAGE) {
        *__errno() = EINVAL; /* matches real ftruncate() on a non-regular-file fd */
        return -1;
    }
    if (storage__truncate(box->file, (uint64_t)length) != BRUCE_OK) {
        *__errno() = EIO;
        return -1;
    }
    return 0;
}

static void bruce_elf__stat_fill(struct stat *out, bool is_dir, size_t size) {
    memset(out, 0, sizeof(*out));
    out->st_mode = (mode_t)((is_dir ? S_IFDIR : S_IFREG) | (is_dir ? 0755 : 0644));
    out->st_nlink = 1;
    out->st_size = (off_t)size;
    out->st_blksize = 512;
    out->st_blocks = (blkcnt_t)((size + 511) / 512);
}

int bruce_elf__stat(const char *path, struct stat *out) {
    if (path == NULL || out == NULL) {
        *__errno() = EINVAL;
        return -1;
    }
    size_t entry_count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &entry_count);
    if (result == BRUCE_OK) {
        bruce_elf__stat_fill(out, true, 0);
        return 0;
    }
    if (result != BRUCE_ERR_IO) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    bruce_file_id_t file;
    result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    uint64_t size = 0;
    storage__seek(file, 0, SEEK_END, &size);
    storage__close(file);
    bruce_elf__stat_fill(out, false, (size_t)size);
    return 0;
}

int bruce_elf__fstat(int fd, struct stat *out) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL || out == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (box->kind != BRUCE_ELF_FILE_STORAGE) {
        /* stdin/stdout/stderr: report as a character device, no seekable size. */
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFCHR | 0666;
        out->st_nlink = 1;
        return 0;
    }
    uint64_t saved_position = 0;
    storage__seek(box->file, 0, SEEK_CUR, &saved_position);
    uint64_t size = 0;
    storage__seek(box->file, 0, SEEK_END, &size);
    storage__seek(box->file, (int64_t)saved_position, SEEK_SET, &saved_position);
    bruce_elf__stat_fill(out, false, (size_t)size);
    return 0;
}

int bruce_elf__mkdir(const char *path, mode_t mode) {
    (void)mode; /* BruceOS storage has no permission-bit concept */
    bruce_result_t result = storage__mkdir(path);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    return 0;
}

/* Wraps `raw` in single quotes for app_runner__run()'s own shell-style
 * arg tokenizer (app_runner__parse_args(), core_sdk/app_runner.h): inside
 * a single-quoted span that tokenizer treats everything -- spaces, `"`,
 * `\`, `$`, backticks -- as fully literal, so this is the one wrapping
 * that lets an arbitrary command string survive as exactly one token
 * (the classic close-quote/escaped-quote/reopen-quote splice, '\'', is
 * needed only for a literal `'` inside `raw` itself, since that's the one
 * character this wrapping can't just pass through unescaped). Returns
 * false if `out` is too small, leaving its content undefined. */
static bool bruce_elf__quote_single(const char *raw, char *out, size_t out_capacity) {
    size_t pos = 0;
    if (out_capacity == 0) return false;
#define BRUCE_ELF__QUOTE_PUT(ch)                        \
    do {                                                \
        if (pos + 1 >= out_capacity) return false;      \
        out[pos++] = (ch);                               \
    } while (0)
    BRUCE_ELF__QUOTE_PUT('\'');
    for (const char *p = raw; *p != '\0'; ++p) {
        if (*p == '\'') {
            BRUCE_ELF__QUOTE_PUT('\'');
            BRUCE_ELF__QUOTE_PUT('\\');
            BRUCE_ELF__QUOTE_PUT('\'');
            BRUCE_ELF__QUOTE_PUT('\'');
        } else {
            BRUCE_ELF__QUOTE_PUT(*p);
        }
    }
    BRUCE_ELF__QUOTE_PUT('\'');
#undef BRUCE_ELF__QUOTE_PUT
    out[pos] = '\0';
    return true;
}

/* Runs `command` as "shell -c '<command>'" (see modules/shell/), the same
 * real, separate-process "shell -c" launch every other captured/piped path
 * in that shell already relies on (shell_executor__run_substitution() for
 * "$(...)", command backgrounding, etc.) -- just via the public
 * app_runner__run()/process__wait_status() pair rather than that module's
 * own core-private helpers, since this runs from inside a sandboxed ELF
 * app, not the shell's own process.
 *
 * Unlike those other paths, this doesn't capture output through a
 * stdio__session_*() pipe: BRUCE_LAUNCH_FOREGROUND hands the child the
 * calling process's own routed console session (see
 * stdio__session_route_children()'s doc comment -- nothing here overrides
 * it), so the command's output/input go straight to the real screen/
 * keyboard the caller is already attached to, matching plain system()'s
 * usual behavior (and, concretely, what vi's ":!cmd" wants: the user
 * watches the command run live, not a buffered result after the fact).
 *
 * On any failure to even launch or wait for the child, returns -1 with
 * *__errno() set (ENOMEM for a too-long command, EIO otherwise); a
 * successfully completed run always returns the real child's exit code,
 * even 0 -- there's no separate way to report "launch itself failed"
 * within that range the way a real fork()+exec()-based system() briefly
 * has via WIFSIGNALED etc., so a killed/terminated child's status is
 * folded into -1 too. system(NULL) reports a command processor as
 * available (a nonzero/true return), per the standard's own probe
 * convention -- this sandbox always has one (modules/shell/). */
int bruce_elf__system(const char *command) {
    if (command == NULL) return 1;

    size_t command_len = strlen(command);
    /* Worst case every byte is a `'` needing the 4-byte splice, plus the
     * two wrapping quotes, the "-c " prefix, and the NUL. */
    size_t capacity = command_len * 4u + 8u;
    char *quoted = malloc(capacity);
    if (quoted == NULL) {
        *__errno() = ENOMEM;
        return -1;
    }
    memcpy(quoted, "-c ", 3);
    if (!bruce_elf__quote_single(command, quoted + 3, capacity - 3)) {
        free(quoted);
        *__errno() = ENOMEM; /* only reachable if the capacity math above is ever wrong */
        return -1;
    }

    int launched = app_runner__run("shell", quoted, BRUCE_LAUNCH_FOREGROUND);
    free(quoted);
    if (launched <= 0) {
        *__errno() = EIO;
        return -1;
    }

    bruce_process_status_t status = {0};
    bruce_result_t waited = process__wait_status((bruce_process_id_t)launched, UINT32_MAX, &status);
    if (waited != BRUCE_OK || status.reason != BRUCE_PROCESS_EXITED) {
        *__errno() = EIO;
        return -1;
    }
    return status.exit_code;
}

int bruce_elf__access(const char *path, int mode) {
    (void)mode; /* only existence is checked -- no read/write/execute permission bits here */
    bool exists = false;
    bruce_result_t result = storage__exists(path, &exists);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    if (!exists) {
        *__errno() = ENOENT;
        return -1;
    }
    return 0;
}

/* One malloc'd snapshot of storage__list()'s output, walked by index --
 * see the design comment above for why this can't be a live cursor. */
typedef struct {
    bruce_storage_entry_t *entries;
    size_t count;
    size_t index;
    struct dirent current;
} bruce_elf_dir_t;

DIR *bruce_elf__opendir(const char *path) {
    if (path == NULL) {
        *__errno() = EINVAL;
        return NULL;
    }
    size_t total = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &total);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return NULL;
    }
    bruce_elf_dir_t *dir = memory__malloc(sizeof(*dir));
    if (dir == NULL) {
        *__errno() = ENOMEM;
        return NULL;
    }
    dir->entries = NULL;
    dir->count = 0;
    dir->index = 0;
    if (total > 0) {
        dir->entries = memory__malloc(total * sizeof(bruce_storage_entry_t));
        if (dir->entries == NULL) {
            memory__free(dir);
            *__errno() = ENOMEM;
            return NULL;
        }
        size_t actual = 0;
        result = storage__list(path, dir->entries, total, &actual);
        if (result != BRUCE_OK) {
            memory__free(dir->entries);
            memory__free(dir);
            bruce_elf__set_errno_from_result(result);
            return NULL;
        }
        dir->count = actual < total ? actual : total;
    }
    return (DIR *)dir;
}

struct dirent *bruce_elf__readdir(DIR *dirp) {
    bruce_elf_dir_t *dir = (bruce_elf_dir_t *)dirp;
    if (dir == NULL || dir->index >= dir->count) return NULL;
    const bruce_storage_entry_t *entry = &dir->entries[dir->index++];
    memset(&dir->current, 0, sizeof(dir->current));
    dir->current.d_type = (entry->type == BRUCE_STORAGE_ENTRY_DIRECTORY) ? DT_DIR : DT_REG;
    size_t name_len = strlen(entry->name);
    if (name_len >= sizeof(dir->current.d_name)) name_len = sizeof(dir->current.d_name) - 1;
    memcpy(dir->current.d_name, entry->name, name_len);
    dir->current.d_name[name_len] = '\0';
    return &dir->current;
}

void bruce_elf__rewinddir(DIR *dirp) {
    bruce_elf_dir_t *dir = (bruce_elf_dir_t *)dirp;
    if (dir != NULL) dir->index = 0;
}

int bruce_elf__closedir(DIR *dirp) {
    bruce_elf_dir_t *dir = (bruce_elf_dir_t *)dirp;
    if (dir == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    memory__free(dir->entries);
    memory__free(dir);
    return 0;
}

/* ---------------------------------------------------------------------------
 * BSD sockets (socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/
 * shutdown/setsockopt/getsockopt), for unmodified third-party C code written
 * against the standard sockets API rather than tcp__/udp__ directly -- an ELF
 * app gets these by adding `PRIV_REQUIRES lwip` to its own
 * idf_component_register() and `#include <sys/socket.h>` (there is no
 * picolibc header for this -- verified directly, picolibc ships no
 * sys/socket.h/netinet/in.h/arpa/inet.h at all -- so the app's own build has
 * to pull the real ones in from ESP-IDF's lwip component, same as any other
 * normal ESP-IDF project using sockets).
 *
 * Registered below under "lwip_socket"/"lwip_bind"/etc, NOT "socket"/"bind"/
 * etc: that same lwip/sockets.h defines LWIP_COMPAT_SOCKETS, whose default
 * value (1 -- checked directly in this project's actual lwip/opt.h, not
 * assumed) makes `socket(...)` etc. preprocessor macros that expand to
 * `lwip_socket(...)` etc. at the APP's OWN compile time, so the relocation
 * a compiled ELF app actually carries is for "lwip_socket", never "socket"
 * itself. close()/read()/write() are the one exception -- LWIP_COMPAT_SOCKETS
 * only remaps those two under the rarer ==2 setting -- so a socket fd's
 * close()/read()/write() calls normally reach the plain "close"/"read"/
 * "write" entries already registered above for storage fds, which is why
 * bruce_elf__close()/read()/write() were extended (see the kind dispatch
 * added to each) to also understand a socket-kind fd rather than adding yet
 * another parallel set of adapters for those three. "lwip_close"/"lwip_read"/
 * "lwip_write" are registered too, pointing at those same three functions,
 * purely as insurance for an ELF app built with LWIP_COMPAT_SOCKETS==2 in
 * its own sdkconfig instead of the default.
 *
 * Built entirely on core_sdk/tcp.h and core_sdk/udp.h -- this file never
 * touches a raw lwip fd directly for any of this, only lwip's struct/constant
 * definitions (struct sockaddr_in, AF_INET, ...) and its pure address-format
 * helpers (inet_ntop/pton and friends, exported unadapted further below,
 * same rationale as strerror()/div() elsewhere in this file) -- so every BSD
 * socket still goes through the same `wifi` permission check, per-process
 * ownership, and auto-close-on-exit that tcp__/udp__ already enforce for
 * every other network entry point in this sandbox. Calling straight into
 * lwip's own socket()/connect()/etc. here would silently punch a hole through
 * all of that for any app that happens to spell its networking the BSD way
 * instead of BruceOS's own.
 *
 * Only AF_INET/SOCK_STREAM/SOCK_DGRAM are supported -- this whole sandbox's
 * network layer is IPv4-only already (see bruce_tcp_endpoint_t/
 * bruce_udp_endpoint_t) -- no AF_INET6, no AF_UNIX, no SOCK_RAW.
 *
 * A BSD socket has staged setup (socket() now, connect() OR bind()+listen()
 * later) that tcp__/udp__'s own connect()-does-everything/listen()-does-
 * everything calls don't have, so a socket's box (bruce_elf_file_t, the same
 * boxed-fd type the FILE* / POSIX-fd families above already use) can sit in
 * BRUCE_ELF_FILE_SOCKET_TCP_PENDING -- allocated by socket(), no real tcp__
 * handle yet -- until connect() or bind()+listen() actually creates one.
 * listen() specifically requires a prior bind() to an explicit port: real
 * listen() on a never-bound socket picks an ephemeral port automatically,
 * but tcp__listen() has no such mode (it always requires a caller-chosen
 * port) -- a real, documented limitation, not a bug, and not one that
 * affects any TCP server example this sandbox is meant to run (they already
 * bind() to a fixed port). UDP has no such staging in real BSD sockets (a UDP
 * socket can send immediately without ever calling bind()), so
 * socket(SOCK_DGRAM) opens a real udp__ handle immediately on an ephemeral
 * port, exactly like the kernel silently auto-binding on first use; a later
 * explicit bind() closes that ephemeral handle and reopens on the caller's
 * chosen port instead (see bruce_elf__bind()) -- unlike a real kernel this
 * doesn't refuse a bind() after the socket has already sent or received, a
 * known simplification, since tracking that would mean threading a "used"
 * flag through every send/recv path for a case real portable code almost
 * never actually hits (bind(), when a program calls it at all, is always the
 * first thing it does to a fresh UDP socket).
 *
 * No non-blocking mode: there is no fcntl()/O_NONBLOCK support in this
 * sandbox, so connect()/accept()/send()/recv()/sendto()/recvfrom() all block
 * (internally retrying the matching tcp__/udp__ call on BRUCE_ERR_TIMEOUT in
 * a loop) until they succeed or hit a real error -- the correct behavior for
 * a blocking socket, which is the only kind this sandbox has. The one
 * exception is the standard MSG_DONTWAIT flag on send/recv/sendto/recvfrom,
 * honored per-call without needing O_NONBLOCK on the fd at all. There is also
 * no select()/poll(): both need to wait on several fds at once, which has no
 * equivalent in tcp__/udp__'s own single-socket wait -- a caller that needs
 * multiplexing has no substitute here yet.
 *
 * shutdown() and setsockopt()/getsockopt() are documented no-ops (like
 * fflush()/setvbuf() above) rather than faked: tcp__/udp__ have no partial-
 * close or socket-option primitives underneath for these to actually drive,
 * so they just validate the fd and satisfy the ABI -- getsockopt(SO_ERROR)
 * specifically always reports "no error" since every call here already
 * blocks to completion rather than leaving an async error to be collected
 * later. */

static void bruce_elf__sockaddr_from_endpoint(struct sockaddr_in *out, const char *host, uint16_t port) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    if (host == NULL || host[0] == '\0' || inet_pton(AF_INET, host, &out->sin_addr) != 1) {
        out->sin_addr.s_addr = htonl(INADDR_ANY);
    }
}

static bruce_result_t bruce_elf__endpoint_from_sockaddr(
    const struct sockaddr *addr, socklen_t addrlen, char *out_host, size_t host_size, uint16_t *out_port
) {
    if (addr == NULL || addrlen < (socklen_t)sizeof(struct sockaddr_in) || addr->sa_family != AF_INET) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
    if (inet_ntop(AF_INET, &in->sin_addr, out_host, host_size) == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_port = ntohs(in->sin_port);
    return BRUCE_OK;
}

/* Shared by bruce_elf__send()/bruce_elf__sendto() (a plain send() is exactly
 * a sendto() with no destination) and by bruce_elf__write() dispatching a
 * socket-kind fd to bruce_elf__socket_write() below. size == 0 is handled
 * before ever calling tcp__write()/udp__send_to(): both treat capacity/size
 * == 0 as an argument error, but POSIX send()/write() of zero bytes is a
 * valid no-op that returns 0. */
static ssize_t bruce_elf__send_impl(bruce_elf_file_t *box, const void *buffer, size_t size, int flags) {
    if (size == 0) return 0;
    bool poll_once = (flags & MSG_DONTWAIT) != 0;
    for (;;) {
        bruce_result_t result;
        size_t sent = 0;
        if (box->kind == BRUCE_ELF_FILE_SOCKET_TCP_STREAM) {
            result = tcp__write(box->tcp, buffer, size, poll_once ? 0 : 1000, &sent);
        } else if (box->kind == BRUCE_ELF_FILE_SOCKET_UDP && box->udp_connected) {
            result =
                udp__send_to(box->udp, box->udp_peer_host, box->udp_peer_port, buffer, size, poll_once ? 0 : 1000,
                              &sent);
        } else if (box->kind == BRUCE_ELF_FILE_SOCKET_UDP) {
            *__errno() = EDESTADDRREQ; /* no connect() peer, and this isn't sendto() */
            return -1;
        } else {
            *__errno() = ENOTSOCK;
            return -1;
        }
        if (result == BRUCE_OK) return (ssize_t)sent;
        if (result == BRUCE_ERR_TIMEOUT) {
            if (poll_once) {
                *__errno() = EAGAIN;
                return -1;
            }
            continue; /* keep blocking, matching a real blocking socket */
        }
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
}

/* Shared by bruce_elf__recv()/bruce_elf__recvfrom() and by bruce_elf__read()
 * dispatching a socket-kind fd to bruce_elf__socket_read() below. See
 * bruce_elf__send_impl() above for the size == 0 and MSG_DONTWAIT handling,
 * identical here. */
static ssize_t bruce_elf__recv_impl(bruce_elf_file_t *box, void *buffer, size_t size, int flags) {
    if (size == 0) return 0;
    bool poll_once = (flags & MSG_DONTWAIT) != 0;
    for (;;) {
        bruce_result_t result;
        size_t received = 0;
        if (box->kind == BRUCE_ELF_FILE_SOCKET_TCP_STREAM) {
            result = tcp__read(box->tcp, buffer, size, poll_once ? 0 : 1000, &received);
        } else if (box->kind == BRUCE_ELF_FILE_SOCKET_UDP) {
            /* Unlike send()/sendto(), recv()/recvfrom() never require a
             * connect()ed peer -- real recv() is just recvfrom(..., NULL,
             * NULL), and recvfrom() on a never-connected UDP socket happily
             * accepts a datagram from anyone. connect() only narrows this:
             * once set, the peer filter below discards anything else,
             * matching real connected-UDP delivery (the kernel itself drops
             * non-matching datagrams for a connected socket, for recv() and
             * recvfrom() alike). */
            bruce_udp_endpoint_t sender;
            result = udp__receive_from(box->udp, buffer, size, poll_once ? 0 : 1000, &received, &sender);
            if (box->udp_connected && result == BRUCE_OK &&
                (strcmp(sender.host, box->udp_peer_host) != 0 || sender.port != box->udp_peer_port)) {
                continue; /* wrong sender -- keep waiting, this datagram is discarded */
            }
        } else {
            *__errno() = ENOTSOCK;
            return -1;
        }
        if (result == BRUCE_OK) return (ssize_t)received;
        if (result == BRUCE_ERR_TIMEOUT) {
            if (poll_once) {
                *__errno() = EAGAIN;
                return -1;
            }
            continue;
        }
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
}

static int bruce_elf__socket_close(bruce_elf_file_t *box) {
    bruce_result_t result = BRUCE_OK;
    switch (box->kind) {
        case BRUCE_ELF_FILE_SOCKET_TCP_STREAM:
        case BRUCE_ELF_FILE_SOCKET_TCP_LISTENER: result = tcp__close(box->tcp); break;
        case BRUCE_ELF_FILE_SOCKET_UDP: result = udp__close(box->udp); break;
        default: break; /* TCP_PENDING: never had a real tcp__/udp__ handle to close */
    }
    memory__free(box);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    return 0;
}

static ssize_t bruce_elf__socket_read(bruce_elf_file_t *box, void *buffer, size_t count) {
    return bruce_elf__recv_impl(box, buffer, count, 0);
}

static ssize_t bruce_elf__socket_write(bruce_elf_file_t *box, const void *buffer, size_t count) {
    return bruce_elf__send_impl(box, buffer, count, 0);
}

int bruce_elf__socket(int domain, int type, int protocol) {
    (void)protocol;
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) {
        bruce_elf__set_errno_from_result(permission);
        return -1;
    }
    if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM)) {
        *__errno() = EINVAL;
        return -1;
    }
    bruce_elf_file_t *box = memory__malloc(sizeof(*box));
    if (box == NULL) {
        *__errno() = ENOMEM;
        return -1;
    }
    memset(box, 0, sizeof(*box));
    if (type == SOCK_STREAM) {
        box->kind = BRUCE_ELF_FILE_SOCKET_TCP_PENDING;
    } else {
        bruce_result_t result = udp__open(0, &box->udp);
        if (result != BRUCE_OK) {
            memory__free(box);
            bruce_elf__set_errno_from_result(result);
            return -1;
        }
        box->kind = BRUCE_ELF_FILE_SOCKET_UDP;
    }
    int fd = bruce_elf__fd_alloc(box);
    if (fd < 0) {
        if (box->kind == BRUCE_ELF_FILE_SOCKET_UDP) udp__close(box->udp);
        memory__free(box);
        *__errno() = EMFILE;
        return -1;
    }
    return fd;
}

int bruce_elf__bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    char host[BRUCE_UDP_HOST_MAX];
    uint16_t port = 0;
    if (bruce_elf__endpoint_from_sockaddr(addr, addrlen, host, sizeof(host), &port) != BRUCE_OK) {
        *__errno() = EINVAL;
        return -1;
    }
    if (box->kind == BRUCE_ELF_FILE_SOCKET_TCP_PENDING) {
        box->pending_port = port;
        return 0;
    }
    if (box->kind == BRUCE_ELF_FILE_SOCKET_UDP) {
        if (box->udp_connected) {
            *__errno() = EINVAL; /* real bind() after connect() also fails */
            return -1;
        }
        bruce_udp_id_t rebound = BRUCE_UDP_ID_INVALID;
        bruce_result_t result = udp__open(port, &rebound);
        if (result != BRUCE_OK) {
            bruce_elf__set_errno_from_result(result);
            return -1;
        }
        udp__close(box->udp); /* only after the reopen succeeds -- see the doc comment above */
        box->udp = rebound;
        return 0;
    }
    *__errno() = EINVAL;
    return -1;
}

int bruce_elf__listen(int fd, int backlog) {
    (void)backlog; /* tcp__listen() always uses a backlog of 1 internally */
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (box->kind != BRUCE_ELF_FILE_SOCKET_TCP_PENDING || box->pending_port == 0) {
        *__errno() = EDESTADDRREQ; /* see the doc comment above: bind() to an explicit port is mandatory first */
        return -1;
    }
    bruce_tcp_id_t listener = BRUCE_TCP_ID_INVALID;
    bruce_result_t result = tcp__listen(box->pending_port, &listener);
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    box->tcp = listener;
    box->kind = BRUCE_ELF_FILE_SOCKET_TCP_LISTENER;
    return 0;
}

int bruce_elf__connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    char host[BRUCE_UDP_HOST_MAX];
    uint16_t port = 0;
    if (bruce_elf__endpoint_from_sockaddr(addr, addrlen, host, sizeof(host), &port) != BRUCE_OK) {
        *__errno() = EINVAL;
        return -1;
    }
    if (box->kind == BRUCE_ELF_FILE_SOCKET_TCP_PENDING) {
        bruce_tcp_id_t id = BRUCE_TCP_ID_INVALID;
        bruce_result_t result = tcp__connect(host, port, 0, &id);
        if (result != BRUCE_OK) {
            bruce_elf__set_errno_from_result(result);
            return -1;
        }
        box->tcp = id;
        box->kind = BRUCE_ELF_FILE_SOCKET_TCP_STREAM;
        return 0;
    }
    if (box->kind == BRUCE_ELF_FILE_SOCKET_UDP) {
        strncpy(box->udp_peer_host, host, sizeof(box->udp_peer_host) - 1);
        box->udp_peer_host[sizeof(box->udp_peer_host) - 1] = '\0';
        box->udp_peer_port = port;
        box->udp_connected = true;
        return 0;
    }
    *__errno() = box->kind == BRUCE_ELF_FILE_SOCKET_TCP_STREAM ? EISCONN : EINVAL;
    return -1;
}

int bruce_elf__accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (box->kind != BRUCE_ELF_FILE_SOCKET_TCP_LISTENER) {
        *__errno() = EINVAL;
        return -1;
    }
    bruce_tcp_id_t accepted = BRUCE_TCP_ID_INVALID;
    bruce_tcp_endpoint_t peer;
    bruce_result_t result;
    do {
        result = tcp__accept(box->tcp, 1000, &accepted, &peer);
    } while (result == BRUCE_ERR_TIMEOUT); /* block indefinitely -- see the doc comment above */
    if (result != BRUCE_OK) {
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
    bruce_elf_file_t *accepted_box = memory__malloc(sizeof(*accepted_box));
    if (accepted_box == NULL) {
        tcp__close(accepted);
        *__errno() = ENOMEM;
        return -1;
    }
    memset(accepted_box, 0, sizeof(*accepted_box));
    accepted_box->kind = BRUCE_ELF_FILE_SOCKET_TCP_STREAM;
    accepted_box->tcp = accepted;
    int accepted_fd = bruce_elf__fd_alloc(accepted_box);
    if (accepted_fd < 0) {
        tcp__close(accepted);
        memory__free(accepted_box);
        *__errno() = EMFILE;
        return -1;
    }
    if (addr != NULL && addrlen != NULL && *addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
        bruce_elf__sockaddr_from_endpoint((struct sockaddr_in *)addr, peer.host, peer.port);
        *addrlen = (socklen_t)sizeof(struct sockaddr_in);
    }
    return accepted_fd;
}

ssize_t bruce_elf__send(int fd, const void *buffer, size_t size, int flags) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    return bruce_elf__send_impl(box, buffer, size, flags);
}

ssize_t bruce_elf__recv(int fd, void *buffer, size_t size, int flags) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    return bruce_elf__recv_impl(box, buffer, size, flags);
}

ssize_t bruce_elf__sendto(
    int fd, const void *buffer, size_t size, int flags, const struct sockaddr *to, socklen_t tolen
) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (to == NULL) return bruce_elf__send_impl(box, buffer, size, flags);
    if (box->kind != BRUCE_ELF_FILE_SOCKET_UDP) {
        *__errno() = EINVAL; /* explicit destination only makes sense for UDP here */
        return -1;
    }
    char host[BRUCE_UDP_HOST_MAX];
    uint16_t port = 0;
    if (bruce_elf__endpoint_from_sockaddr(to, tolen, host, sizeof(host), &port) != BRUCE_OK) {
        *__errno() = EINVAL;
        return -1;
    }
    if (size == 0) return 0;
    bool poll_once = (flags & MSG_DONTWAIT) != 0;
    for (;;) {
        size_t sent = 0;
        bruce_result_t result = udp__send_to(box->udp, host, port, buffer, size, poll_once ? 0 : 1000, &sent);
        if (result == BRUCE_OK) return (ssize_t)sent;
        if (result == BRUCE_ERR_TIMEOUT) {
            if (poll_once) {
                *__errno() = EAGAIN;
                return -1;
            }
            continue;
        }
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
}

ssize_t bruce_elf__recvfrom(
    int fd, void *buffer, size_t size, int flags, struct sockaddr *from, socklen_t *fromlen
) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (from == NULL) return bruce_elf__recv_impl(box, buffer, size, flags);
    if (box->kind != BRUCE_ELF_FILE_SOCKET_UDP) {
        *__errno() = EINVAL;
        return -1;
    }
    if (size == 0) return 0;
    bool poll_once = (flags & MSG_DONTWAIT) != 0;
    bruce_udp_endpoint_t peer;
    for (;;) {
        size_t received = 0;
        bruce_result_t result =
            udp__receive_from(box->udp, buffer, size, poll_once ? 0 : 1000, &received, &peer);
        if (result == BRUCE_OK) {
            if (fromlen != NULL && *fromlen >= (socklen_t)sizeof(struct sockaddr_in)) {
                bruce_elf__sockaddr_from_endpoint((struct sockaddr_in *)from, peer.host, peer.port);
                *fromlen = (socklen_t)sizeof(struct sockaddr_in);
            }
            return (ssize_t)received;
        }
        if (result == BRUCE_ERR_TIMEOUT) {
            if (poll_once) {
                *__errno() = EAGAIN;
                return -1;
            }
            continue;
        }
        bruce_elf__set_errno_from_result(result);
        return -1;
    }
}

int bruce_elf__shutdown(int fd, int how) {
    (void)how;
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (!bruce_elf__file_kind_is_socket(box->kind)) {
        *__errno() = ENOTSOCK;
        return -1;
    }
    return 0; /* no-op -- see the doc comment above */
}

int bruce_elf__setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (!bruce_elf__file_kind_is_socket(box->kind)) {
        *__errno() = ENOTSOCK;
        return -1;
    }
    return 0; /* accepted, not applied -- see the doc comment above */
}

int bruce_elf__getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    bruce_elf_file_t *box = bruce_elf__fd_lookup(fd);
    if (box == NULL) {
        *__errno() = EBADF;
        return -1;
    }
    if (!bruce_elf__file_kind_is_socket(box->kind)) {
        *__errno() = ENOTSOCK;
        return -1;
    }
    if (optval != NULL && optlen != NULL && *optlen >= sizeof(int)) {
        /* SO_ERROR or anything else: always "no error"/zero -- see the doc
         * comment above for why there is never a pending one to report. */
        *(int *)optval = 0;
        *optlen = sizeof(int);
    }
    return 0;
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
 * full C runtime exit() call; app_main returning is the normal ELF-app
 * teardown path (the loader reclaims the process's memory directly -- see
 * the .init_array comment above), and even now that exit()/abort() exist
 * below, calling out of the sandbox that way is still just a longjmp(), not
 * a real C runtime driving exit() -- there is nothing useful to register
 * either way. A no-op that reports success satisfies the ABI contract
 * without pretending to actually run anything later.
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

    /* stdlib.h process termination -- see the comment above
     * bruce_elf__exit_common() for what these actually do in a sandbox with
     * no real process boundary. _exit aliases the exact same function as
     * exit: the distinction (skip atexit() handlers) is meaningless here,
     * since neither one ever runs them. */
    {"exit",  (const void *)&bruce_elf__exit },
    {"_exit", (const void *)&bruce_elf__exit },
    {"abort", (const void *)&bruce_elf__abort},

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

    /* Partition layout. list_current()/list_planned()/status() are readable
     * by any process; stage_create()/stage_delete()/stage_format()/commit()/
     * discard() enforce "built-in only" themselves
     * (partition_manager__caller_is_built_in(), core/partition_manager/
     * partition_manager.c) via the same process_registry__current_context()
     * check permission__check() uses -- an ELF app is never a built-in
     * process, so those calls fail closed with BRUCE_ERR_PERMISSION here
     * exactly as they would for any other non-built-in caller. Exported
     * unconditionally rather than only the read side, so that failure is
     * this file's normal permission handling rather than an unresolved
     * symbol. */
    ESP_ELFSYM_EXPORT(partition_manager__list_current),
    ESP_ELFSYM_EXPORT(partition_manager__list_planned),
    ESP_ELFSYM_EXPORT(partition_manager__status),
    ESP_ELFSYM_EXPORT(partition_manager__stage_create),
    ESP_ELFSYM_EXPORT(partition_manager__stage_delete),
    ESP_ELFSYM_EXPORT(partition_manager__stage_format),
    ESP_ELFSYM_EXPORT(partition_manager__commit),
    ESP_ELFSYM_EXPORT(partition_manager__discard),

    /* Per-app JSON settings. app_name == NULL always resolves to the
     * caller's own identity (core/app_config/app_config.c sanitizes it into
     * a legal name), so a sandboxed ELF app can safely store its own
     * settings this way; an explicit app_name naming some OTHER app is
     * gated to built-in processes only, the same way partition_manager's
     * mutating calls are above -- an ELF app that tries it gets treated
     * exactly like an invalid name (see core_sdk/app_config.h). */
    ESP_ELFSYM_EXPORT(app_config__get_bool),
    ESP_ELFSYM_EXPORT(app_config__set_bool),
    ESP_ELFSYM_EXPORT(app_config__get_int),
    ESP_ELFSYM_EXPORT(app_config__set_int),
    ESP_ELFSYM_EXPORT(app_config__get_string),
    ESP_ELFSYM_EXPORT(app_config__set_string),
    ESP_ELFSYM_EXPORT(app_config__get_json),
    ESP_ELFSYM_EXPORT(app_config__set_json),
    ESP_ELFSYM_EXPORT(app_config__get_bool_array),
    ESP_ELFSYM_EXPORT(app_config__set_bool_array),
    ESP_ELFSYM_EXPORT(app_config__get_int_array),
    ESP_ELFSYM_EXPORT(app_config__set_int_array),
    ESP_ELFSYM_EXPORT(app_config__get_string_array),
    ESP_ELFSYM_EXPORT(app_config__set_string_array),
    ESP_ELFSYM_EXPORT(app_config__remove),

    /* Hashing/checksums, base64, DEFLATE compression, and tar.gz/zip
     * archives -- exported directly, unlike the storage__/tcp__/udp__
     * families above: none of these hand back a handle that needs boxing
     * into a small-int fd or tracking for auto-cleanup-on-exit. A
     * bruce_hash_ctx_t* / bruce_compress_ctx_t* is just heap memory the
     * calling process already owns and must pass back to *_finish() / *_end()
     * itself, exactly like a malloc()'d buffer -- no different from any
     * other pointer already crossing this boundary. archive__*_list()'s
     * callback parameter is a plain function pointer into the ELF app's own
     * (already-relocated) code, which core/archive calls directly; nothing
     * about crossing the ELF boundary is specific to it being a callback. */
    ESP_ELFSYM_EXPORT(hash__start),
    ESP_ELFSYM_EXPORT(hash__update),
    ESP_ELFSYM_EXPORT(hash__finish),
    ESP_ELFSYM_EXPORT(hash__compute),
    ESP_ELFSYM_EXPORT(hash__crc32),
    ESP_ELFSYM_EXPORT(base64__encode),
    ESP_ELFSYM_EXPORT(base64__decode),
    ESP_ELFSYM_EXPORT(compress__bound),
    ESP_ELFSYM_EXPORT(compress__compute),
    ESP_ELFSYM_EXPORT(decompress__compute),
    ESP_ELFSYM_EXPORT(compress__start),
    ESP_ELFSYM_EXPORT(compress__update),
    ESP_ELFSYM_EXPORT(compress__end),
    ESP_ELFSYM_EXPORT(decompress__start),
    ESP_ELFSYM_EXPORT(decompress__update),
    ESP_ELFSYM_EXPORT(decompress__end),
    ESP_ELFSYM_EXPORT(archive__tar_gz_create),
    ESP_ELFSYM_EXPORT(archive__tar_gz_list),
    ESP_ELFSYM_EXPORT(archive__tar_gz_extract),
    ESP_ELFSYM_EXPORT(archive__tar_gz_extract_entry),
    ESP_ELFSYM_EXPORT(archive__tar_gz_read_entry),
    ESP_ELFSYM_EXPORT(archive__zip_create),
    ESP_ELFSYM_EXPORT(archive__zip_list),
    ESP_ELFSYM_EXPORT(archive__zip_extract),
    ESP_ELFSYM_EXPORT(archive__zip_extract_entry),
    ESP_ELFSYM_EXPORT(archive__zip_read_entry),

    /* Clipboard */
    ESP_ELFSYM_EXPORT(clipboard__kind),
    ESP_ELFSYM_EXPORT(clipboard__set_text),
    ESP_ELFSYM_EXPORT(clipboard__get_text),
    ESP_ELFSYM_EXPORT(clipboard__set_files),
    ESP_ELFSYM_EXPORT(clipboard__file_count),
    ESP_ELFSYM_EXPORT(clipboard__get_file),
    ESP_ELFSYM_EXPORT(clipboard__file_mode),
    ESP_ELFSYM_EXPORT(clipboard__paste_files),
    ESP_ELFSYM_EXPORT(clipboard__paste_file_as),
    ESP_ELFSYM_EXPORT(clipboard__set_binary),
    ESP_ELFSYM_EXPORT(clipboard__binary_size),
    ESP_ELFSYM_EXPORT(clipboard__get_binary),
    ESP_ELFSYM_EXPORT(clipboard__binary_filename),
    ESP_ELFSYM_EXPORT(clipboard__paste_binary),
    ESP_ELFSYM_EXPORT(clipboard__clear),

    /* File type identification */
    ESP_ELFSYM_EXPORT(filetype__lookup_extension),
    ESP_ELFSYM_EXPORT(filetype__icon_for_path),
    ESP_ELFSYM_EXPORT(filetype__identify),
    ESP_ELFSYM_EXPORT(filetype__identify_bytes),

    /* TCP, UDP, and console streams */
    ESP_ELFSYM_EXPORT(tcp__connect),
    ESP_ELFSYM_EXPORT(tcp__listen),
    ESP_ELFSYM_EXPORT(tcp__accept),
    ESP_ELFSYM_EXPORT(tcp__read),
    ESP_ELFSYM_EXPORT(tcp__write),
    ESP_ELFSYM_EXPORT(tcp__close),
    ESP_ELFSYM_EXPORT(udp__open),
    ESP_ELFSYM_EXPORT(udp__send_to),
    ESP_ELFSYM_EXPORT(udp__receive_from),
    ESP_ELFSYM_EXPORT(udp__close),
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
    {"puts",      (const void *)&bruce_elf__puts     },
    {"putchar",   (const void *)&bruce_elf__putchar  },
    {"getuid",    (const void *)&bruce_elf__getuid   },
    {"strchrnul", (const void *)&bruce_elf__strchrnul},

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
    {"perror",   (const void *)&bruce_elf__perror   },
    {"setvbuf",  (const void *)&bruce_elf__setvbuf  },
    {"open",     (const void *)&bruce_elf__open     },
    {"close",    (const void *)&bruce_elf__close    },
    {"read",     (const void *)&bruce_elf__read     },
    {"write",    (const void *)&bruce_elf__write    },
    {"lseek",    (const void *)&bruce_elf__lseek    },
    {"ftruncate",(const void *)&bruce_elf__ftruncate},
    {"stat",     (const void *)&bruce_elf__stat     },
    {"fstat",    (const void *)&bruce_elf__fstat    },
    {"mkdir",    (const void *)&bruce_elf__mkdir    },
    {"system",   (const void *)&bruce_elf__system   },
    {"access",   (const void *)&bruce_elf__access   },
    {"unlink",   (const void *)&bruce_elf__remove   }, /* unlink() and remove() are the same operation here */
    {"opendir",  (const void *)&bruce_elf__opendir  },
    {"readdir",  (const void *)&bruce_elf__readdir  },
    {"closedir", (const void *)&bruce_elf__closedir },
    {"rewinddir",(const void *)&bruce_elf__rewinddir},

    /* BSD sockets, backed by core_sdk/tcp.h and core_sdk/udp.h -- see the
     * design doc comment above bruce_elf__sockaddr_from_endpoint(). Registered
     * under "lwip_*" names (plus a few unprefixed pure-function/struct-helper
     * names below), NOT the plain POSIX names: LWIP_COMPAT_SOCKETS (default
     * on) makes socket()/bind()/connect()/etc. expand to lwip_socket()/
     * lwip_bind()/lwip_connect()/etc. at the calling ELF app's own compile
     * time, so those are the actual relocations a compiled app carries.
     * close()/read()/write() are the exception -- only remapped under the
     * rarer LWIP_COMPAT_SOCKETS==2 -- so "close"/"read"/"write" above were
     * extended to also dispatch a socket-kind fd; "lwip_close"/"lwip_read"/
     * "lwip_write" here point at those same three functions, for an app built
     * with ==2 instead of the default. */
    {"lwip_socket",     (const void *)&bruce_elf__socket    },
    {"lwip_bind",       (const void *)&bruce_elf__bind      },
    {"lwip_listen",     (const void *)&bruce_elf__listen    },
    {"lwip_connect",    (const void *)&bruce_elf__connect   },
    {"lwip_accept",     (const void *)&bruce_elf__accept    },
    {"lwip_send",       (const void *)&bruce_elf__send      },
    {"lwip_recv",       (const void *)&bruce_elf__recv      },
    {"lwip_sendto",     (const void *)&bruce_elf__sendto    },
    {"lwip_recvfrom",   (const void *)&bruce_elf__recvfrom  },
    {"lwip_shutdown",   (const void *)&bruce_elf__shutdown  },
    {"lwip_setsockopt", (const void *)&bruce_elf__setsockopt},
    {"lwip_getsockopt", (const void *)&bruce_elf__getsockopt},
    {"lwip_close",      (const void *)&bruce_elf__close     },
    {"lwip_read",       (const void *)&bruce_elf__read      },
    {"lwip_write",      (const void *)&bruce_elf__write     },
    /* htons(x)/ntohl(x) etc are unconditionally macro-aliased to these two
     * real functions (lwip/def.h); on this little-endian target ntohs/ntohl
     * are further macro-aliased to lwip_htons/lwip_htonl themselves, so no
     * separate entries are needed for those two. Pure functions -- exported
     * directly, no adapter, same as strerror()/div() elsewhere in this file. */
    ESP_ELFSYM_EXPORT(lwip_htons),
    ESP_ELFSYM_EXPORT(lwip_htonl),
    /* inet_ntop(...)/inet_pton(...) macro-alias to these (lwip/inet.h),
     * active under the same default LWIP_COMPAT_SOCKETS setting as above. */
    ESP_ELFSYM_EXPORT(lwip_inet_ntop),
    ESP_ELFSYM_EXPORT(lwip_inet_pton),
    /* inet_addr(cp)/inet_aton(cp,addr)/inet_ntoa(addr) macro-alias to these
     * three (lwip/inet.h) unconditionally -- not gated by LWIP_COMPAT_SOCKETS
     * at all, unlike everything else in this block. */
    ESP_ELFSYM_EXPORT(ipaddr_addr),
    ESP_ELFSYM_EXPORT(ip4addr_aton),
    ESP_ELFSYM_EXPORT(ip4addr_ntoa),

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
    ESP_ELFSYM_EXPORT(vsscanf),
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
    ESP_ELFSYM_EXPORT(strerror), /* pure errnum->string lookup, no I/O -- see bruce_elf__perror() above for why perror() itself is not exported this way */
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
    ESP_ELFSYM_EXPORT(div),
    ESP_ELFSYM_EXPORT(ldiv),
    ESP_ELFSYM_EXPORT(lldiv),

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
    ESP_ELFSYM_EXPORT(__muldi3),

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
    /* Not a function, and not ESP_ELFSYM_EXPORT (that macro needs the
     * plain name visible as-is, but our own <ctype.h> never declares this
     * one -- see the hand-written extern + comment near __errno above for
     * why, and why that's fine here anyway). */
    {"_ctype_b", (const void *)&_ctype_b},

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
