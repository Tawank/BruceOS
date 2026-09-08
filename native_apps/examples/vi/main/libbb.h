#pragma once

/* A from-scratch, hand-written stand-in for BusyBox's own libbb.h -- NOT a
 * copy or trimmed fork of it. Real libbb.h is generated against BusyBox's
 * own Kbuild/Kconfig config system and pulls in its entire libbb runtime
 * (string/xfuncs/read_key/procps/... -- thousands of lines across dozens
 * of files); vi.c itself (fetched unmodified, see vi_sources.cmake) only
 * calls a small, finite set of libbb-prefixed helpers, so this declares
 * just those, backed by busybox_shim.c, plus the handful of feature-flag
 * macros vi.c's own #if/#ifdef blocks branch on -- hardcoded to fixed
 * on/off values here instead of coming from a generated config.h. See
 * busybox_shim.c's own top comment for how each helper is implemented and
 * why (stdio__/tty__-backed where it touches the terminal, plain libc
 * where it doesn't).
 *
 * Feature selection. Each ENABLE_FEATURE_VI_* below is paired with an
 * IF_FEATURE_VI_*(...) that expands to its argument list when on, nothing
 * when off -- vi.c uses both forms throughout. Chosen set:
 *  - USE_SIGNALS: OFF. Gates vi.c's only signal()/sigsetjmp()/raise() use
 *    (winch/tstp/int handlers implementing SIGWINCH-triggered redraw and
 *    Ctrl+Z job-control suspend). BruceOS has no SIGWINCH equivalent by
 *    design (core_sdk/tty.h's generation counter is the resize path
 *    instead) and no shell job control to suspend into -- neither
 *    concept has a real counterpart here, so this stays off rather than
 *    faking one. Ctrl+C still reaches vi.c as a plain 0x03 byte (raw mode
 *    passes it through, see core_sdk/tty.h), not a signal.
 *  - WIN_RESIZE: ON. Makes vi.c call the real get_terminal_width_height()
 *    once at startup instead of assuming a fixed 80x24 -- this is the one
 *    piece of "terminal geometry" plumbing that has a direct, better-than
 *    -ioctl counterpart here (tty__get_size(), no escape-sequence
 *    round-trip needed), so it's implemented for real. Without USE_SIGNALS
 *    there's no live mid-session resize (vi.c's own winch_handler is what
 *    would normally requery it), same one-shot-at-open limitation
 *    terminal_app.c's other child processes already live with.
 *  - ASK_TERMINAL: OFF. An ioctl-failed fallback (send "ESC[6n", parse the
 *    cursor-position reply) inside vi.c's own get_terminal_width_height()
 *    call site, moot here since this port's get_terminal_width_height()
 *    (busybox_shim.c) always succeeds via tty__get_size() directly.
 *  - COLON, COLON_EXPAND, SET, SETOPTS, READONLY, SEARCH, DOT_CMD,
 *    UNDO, UNDO_QUEUE, YANKMARK, VERBOSE_STATUS: ON. Core vi editing
 *    features (colon commands, :set, -R, /search, ".", undo, registers,
 *    a nicer status line) -- pure text/array manipulation, nothing
 *    platform-specific, no reason to leave any of these out.
 *  - REGEX_SEARCH: OFF. Needs POSIX regcomp()/regexec()/regfree(), which
 *    this toolchain's C library doesn't provide (no <regex.h> here) --
 *    vi.c's own #else path for this flag falls back to plain substring
 *    search, so disabling it isn't a missing-feature regression so much
 *    as "the smaller of two search implementations vi.c already ships".
 *  - ALLOW_EXEC: ON. Gates ":!cmd" (vi.c's own system() call, the only
 *    thing this flag touches -- see vi.c's own ENABLE_FEATURE_ALLOW_EXEC
 *    block). BruceOS does have a real command processor (modules/shell/)
 *    unlike doom's situation (doom/README.md's bruce_elf__system() note is
 *    about a different, no-shell-to-exec-into case) -- bruce_elf__system()
 *    (elf_loader_sdk_symbols.c) runs the command as a real "shell -c"
 *    child process, console I/O inherited live, exactly what vi.c's own
 *    cookmode()/system()/rawmode()/Hit_Return() sequence around this call
 *    already expects.
 *  - CRASHME: forced OFF unconditionally by vi.c itself already ("the
 *    CRASHME code is unmaintained, and doesn't currently build") --
 *    nothing to set here.
 *  - 8BIT: ON. Only changes the Isprint() byte-range macro (widens it to
 *    accept bytes >= 0x80) -- no function calls gated behind it, so
 *    there's no reason to narrow this port to 7-bit-clean text.
 * LOCALE_SUPPORT (not a FEATURE_VI_* flag, but vi.c branches on it too):
 *  OFF. No wide-char/locale layer here; with 8BIT on above, vi.c's own
 *  non-locale Isprint() branch is already the same permissive byte-range
 *  test, just without a libc locale call backing it. */
#define ENABLE_FEATURE_VI_USE_SIGNALS 0
#define ENABLE_FEATURE_VI_WIN_RESIZE 1
#define ENABLE_FEATURE_VI_ASK_TERMINAL 0
#define ENABLE_FEATURE_VI_COLON 1
#define ENABLE_FEATURE_VI_COLON_EXPAND 1
#define ENABLE_FEATURE_VI_SET 1
#define ENABLE_FEATURE_VI_SETOPTS 1
#define ENABLE_FEATURE_VI_READONLY 1
#define ENABLE_FEATURE_VI_SEARCH 1
#define ENABLE_FEATURE_VI_REGEX_SEARCH 0
#define ENABLE_FEATURE_VI_DOT_CMD 1
#define ENABLE_FEATURE_VI_UNDO 1
#define ENABLE_FEATURE_VI_UNDO_QUEUE 1
#define ENABLE_FEATURE_VI_YANKMARK 1
#define ENABLE_FEATURE_VI_VERBOSE_STATUS 1
#define ENABLE_FEATURE_VI_8BIT 1
#define ENABLE_FEATURE_ALLOW_EXEC 1
#define ENABLE_LOCALE_SUPPORT 0

#define IF_FEATURE_VI_USE_SIGNALS(...)
#define IF_FEATURE_VI_WIN_RESIZE(...) __VA_ARGS__
#define IF_FEATURE_VI_ASK_TERMINAL(...)
#define IF_FEATURE_VI_COLON(...) __VA_ARGS__
#define IF_FEATURE_VI_SETOPTS(...) __VA_ARGS__
#define IF_FEATURE_VI_READONLY(...) __VA_ARGS__
#define IF_FEATURE_VI_SEARCH(...) __VA_ARGS__
#define IF_FEATURE_VI_CRASHME(...)
#define IF_VI(...) __VA_ARGS__

/* Numeric sizing knobs vi.c's own enum{} reads at file scope. Real BusyBox
 * derives these from Config.in prompts (CONFIG_FEATURE_VI_MAX_LEN's
 * default is 4096, CONFIG_FEATURE_VI_UNDO_QUEUE_MAX's is 256) -- reused
 * verbatim, no reason to pick different numbers. */
#define CONFIG_FEATURE_VI_MAX_LEN 4096
#define FEATURE_VI_MAX_LEN CONFIG_FEATURE_VI_MAX_LEN
#define CONFIG_FEATURE_VI_UNDO_QUEUE_MAX 256
#define FEATURE_VI_UNDO_QUEUE_MAX CONFIG_FEATURE_VI_UNDO_QUEUE_MAX

/* ---------------------------------------------------------------------
 * Compiler/linkage attribute macros vi.c decorates declarations with.
 * All plain GCC attributes on this toolchain (Xtensa via GCC) -- BusyBox
 * only special-cases these for MSVC/other compilers this project doesn't
 * target, so there's exactly one definition each rather than the
 * multi-branch #if chain platform.h uses. */
#define FAST_FUNC
#define NORETURN __attribute__((__noreturn__))
#define UNUSED_PARAM __attribute__((__unused__))
#define ALWAYS_INLINE __attribute__((__always_inline__)) inline
#define ALIGN1 __attribute__((aligned(1)))
#define RETURNS_MALLOC __attribute__((malloc))
#define MAIN_EXTERNALLY_VISIBLE

/* This toolchain's <sys/errno.h> has two mutually exclusive forms of
 * "errno": if __PICOLIBC_ERRNO_FUNCTION is defined before this include,
 * "errno" expands to a call through that function pointer/name (safe --
 * it's an ordinary function symbol, resolvable through the Bruce ELF
 * loader's flat name-keyed symbol table exactly like any other SDK call);
 * left undefined (the toolchain's own default), "errno" is instead a real
 * thread-local-storage *variable*, whose every access the compiler lowers
 * to an R_XTENSA_TLS_TPOFF relocation -- a relocation kind the loader has
 * no support for at all (it only resolves ordinary symbol-address
 * relocations), so the ELF fails to load on-device even though it links
 * clean on the build host. Same root-cause bug as doom's original
 * m_misc.c errno issue, except unavoidable here: vi.c itself has one bare
 * "errno" read (readit(), "if (errno == EAGAIN) // paranoia", not gated
 * behind any disabled feature flag) plus several in this shim's own
 * busybox_shim.c, and vi.c is fetched unmodified -- so instead of editing
 * every call site, force the safe form for the whole translation unit.
 * __errno() is already an exported Bruce SDK symbol (elf_loader_sdk_symbols.c,
 * used today by the firmware side to set errno on this app's behalf for
 * failed storage__/socket__ calls), so this is a real, resolvable call,
 * not a new dependency. */
#define __PICOLIBC_ERRNO_FUNCTION __errno
#include <ctype.h> // IWYU pragma: keep
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h> /* struct pollfd, POLLIN -- see busybox_shim.c's safe_poll() */
#include <sys/stat.h>
#include <unistd.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* This toolchain's C library isn't glibc, so there's no "%m" printf
 * extension (glibc's own shortcut for "insert strerror(errno) here") --
 * vi.c's one STRERROR_FMT/STRERROR_ERRNO use (an adjacent-string-literal
 * -plus-injected-comma trick: "text: "STRERROR_FMT STRERROR_ERRNO expands
 * to status_line_bold("text: %s", strerror(errno))) always takes the
 * portable non-glibc form real BusyBox itself falls back to. */
#define STRERROR_FMT "%s"
#define STRERROR_ERRNO ,strerror(errno)

#define BB_VER "BusyBox vi (BruceOS port)"

/* sigjmp_buf backs vi.c's "restart" field (struct globals, unconditional
 * -- only sigsetjmp()/siglongjmp() on it are gated behind
 * ENABLE_FEATURE_VI_USE_SIGNALS, which is off, so the field just sits
 * there unused). This toolchain's <setjmp.h> has jmp_buf but not the
 * POSIX sigsetjmp/siglongjmp signal-mask-saving variant; a plain jmp_buf
 * alias is enough since nothing here ever calls either function. */
#ifndef sigjmp_buf
typedef jmp_buf sigjmp_buf;
#endif

/* "Keycodes" safe_read_key() reports for escape sequences it decodes --
 * small negative values that fit in a signed char yet aren't valid
 * Unicode/ASCII, matching real BusyBox's own scheme (include/libbb.h) so
 * a comment cross-referencing that file stays meaningful. Only the
 * handful vi.c's do_cmd() switch actually references are defined -- it
 * also has an already-#if-0'd-out KEYCODE_FUN1..12 block and never
 * references the Ctrl/Alt/BACKSPACE/D/CURSOR_POS variants real BusyBox
 * defines (checked by grepping the fetched vi.c itself), so those are
 * left out rather than carried along unused. KEYCODE_BUFFER_SIZE sizes
 * vi.c's own "readbuffer" scratch array; safe_read_key() below doesn't
 * actually use its contents (see busybox_shim.c), just needs the array
 * passed in to be at least this big, same as upstream's contract. */
enum {
    KEYCODE_UP = -2,
    KEYCODE_DOWN = -3,
    KEYCODE_RIGHT = -4,
    KEYCODE_LEFT = -5,
    KEYCODE_HOME = -6,
    KEYCODE_END = -7,
    KEYCODE_INSERT = -8,
    KEYCODE_DELETE = -9,
    KEYCODE_PAGEUP = -10,
    KEYCODE_PAGEDOWN = -11,
    KEYCODE_BUFFER_SIZE = 16,
};

/* vi.c's isbackspace() macro reads term_orig.c_cc[VERASE] directly (the
 * only real "struct termios" field access anywhere in it -- checked by
 * grep). There's no real terminal driver underneath (see
 * set_termios_to_raw()'s comment in busybox_shim.c for why), so nothing
 * ever writes a real erase-key byte into this -- it stays zero-filled,
 * which just makes that half of isbackspace()'s "||" always false and
 * falls through to its own hardcoded 8/127 checks, still correct. */
#ifndef VERASE
#define VERASE 2
#endif
struct termios {
    unsigned char c_cc[8];
};
#define TERMIOS_RAW_CRNL_INPUT (1 << 1)
#define TERMIOS_RAW_CRNL_OUTPUT (1 << 2)
#define TERMIOS_RAW_CRNL (TERMIOS_RAW_CRNL_INPUT | TERMIOS_RAW_CRNL_OUTPUT)

/* argv[0]-equivalent BusyBox's multi-applet dispatcher (not present here
 * -- this is a single-purpose ELF app, see main.c) normally sets; used by
 * vi.c only through bb_show_usage()/error messages (busybox_shim.c). */
extern const char *applet_name;

#ifndef TRUE
#define TRUE ((int)1)
#endif

#ifndef FALSE
#define FALSE ((int)0)
#endif

/* BusyBox's space-saving small-int typedefs -- vi.c uses these purely for
 * struct-field size, not for any range/overflow-sensitive arithmetic, so
 * a plain char-sized pair is enough (matches real platform.h's own
 * non-x86 branch). */
typedef signed char smallint;
typedef unsigned char smalluint;

/* vi.c's own INIT_G()/G macro convention: rather than a plain static
 * "struct globals G;", BusyBox mallocs it once and reaches every field
 * through a pointer (ptr_to_globals) -- originally so multiple applets
 * sharing one multi-call busybox binary don't all pay for each other's
 * static state. This app only ever runs one applet, so that motivation
 * doesn't apply, but vi.c's G/SET_PTR_TO_GLOBALS macros still expect the
 * pointer to exist. Untyped here on purpose: vi.c defines its own local
 * "struct globals" *after* including this header, and #define G
 * (*ptr_to_globals) then types every access against that local
 * definition -- same trick real BusyBox's libbb.h uses (see its own
 * struct-globals-per-applet-translation-unit comment), just without that
 * header's extra write-once-after-set hardening
 * (BB_GLOBAL_CONST/ASSIGN_CONST_PTR), which this single-applet app has no
 * reason to carry. A forward declaration is enough for a pointer type;
 * busybox_shim.c's own definition of this variable never needs the
 * complete type either. */
struct globals;
extern struct globals *ptr_to_globals;
#define SET_PTR_TO_GLOBALS(x) (ptr_to_globals = (x))

/* Singly-linked string list -- vi.c only ever reaches it through
 * llist_pop() (never touches ->link/->data directly), so a forward
 * declaration is enough; the real struct lives in busybox_shim.c next to
 * llist_pop() itself. */
typedef struct llist_t llist_t;
char *llist_pop(llist_t **head) FAST_FUNC;

int fputs_stdout(const char *s) FAST_FUNC;

/* vi.c's own getopt32() call site (VI_OPTSTR, see vi.c) only ever needs
 * "Hh" (bare flags) + "R" (bare flag, READONLY on) + "c:*" (COLON on: an
 * argument-taking, repeatable option collected into the llist_t* pointed
 * to by the one variadic argument) -- not general getopt32()'s full
 * option-string grammar (short-opt clusters of arbitrary
 * flag/arg/list/exclusion combinations, long options, etc., none of
 * which this app's option string uses). busybox_shim.c's getopt32()
 * implements exactly that subset against optstring generically (so it
 * doesn't hardcode vi.c's specific option letters), rather than the
 * general form. Consumes argv[1..] up to (not including) the first
 * non-option argument or a NULL sentinel -- main.c always hands vi_main()
 * a NULL-terminated argv, same convention a real OS-provided one has. */
uint32_t getopt32(char **argv, const char *optstring, ...) FAST_FUNC;
extern int optind;

int bb_putchar(int ch) FAST_FUNC;
void bb_show_usage(void) NORETURN FAST_FUNC;
void bb_simple_error_msg_and_die(const char *s) NORETURN FAST_FUNC;
unsigned bb_strtou(const char *arg, char **endp, int base) FAST_FUNC;

int fflush_all(void) FAST_FUNC;

ssize_t full_read(int fd, void *buf, size_t count) FAST_FUNC;
ssize_t full_write(int fd, const void *buf, size_t count) FAST_FUNC;
ssize_t safe_read(int fd, void *buf, size_t count) FAST_FUNC;
int64_t safe_read_key(int fd, char *buffer, int timeout) FAST_FUNC;
int safe_poll(struct pollfd *ufds, unsigned nfds, int timeout) FAST_FUNC;

int set_termios_to_raw(int fd, struct termios *oldterm, int flags) FAST_FUNC;
int tcsetattr_stdin_TCSANOW(const struct termios *tp) FAST_FUNC;
int get_terminal_width_height(int fd, unsigned *width, unsigned *height) FAST_FUNC;

char *skip_whitespace(const char *s) FAST_FUNC;
char *skip_non_whitespace(const char *s) FAST_FUNC;
char *concat_path_file(const char *path, const char *filename) FAST_FUNC RETURNS_MALLOC;
int index_in_strings(const char *strings, const char *key) FAST_FUNC;
/* Not FAST_FUNC/RETURNS_MALLOC-decorated like the others -- this one
 * mirrors the standard (glibc/BSD) memrchr() signature exactly, in case
 * this toolchain's own <string.h> already declares it too (a repeated,
 * compatible extern declaration is legal C either way; if it's not
 * already provided, busybox_shim.c's definition supplies it, and being
 * linked directly into this app rather than sitting in a library archive,
 * it's always the one used regardless of which case applies). */
void *memrchr(const void *s, int c, size_t n);

void *xmalloc(size_t size) FAST_FUNC RETURNS_MALLOC;
void *xzalloc(size_t size) FAST_FUNC RETURNS_MALLOC;
void *xrealloc(void *old, size_t size) FAST_FUNC;
char *xstrdup(const char *s) FAST_FUNC RETURNS_MALLOC;
char *xstrndup(const char *s, size_t n) FAST_FUNC RETURNS_MALLOC;
char *xasprintf(const char *format, ...) __attribute__((format(printf, 1, 2))) FAST_FUNC RETURNS_MALLOC;
void *xmalloc_open_read_close(const char *filename, size_t *maxsz_p) FAST_FUNC RETURNS_MALLOC;
