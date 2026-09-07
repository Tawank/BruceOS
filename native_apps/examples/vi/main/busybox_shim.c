/* Implements the ~20 libbb helpers editors/vi.c (fetched unmodified, see
 * vi_sources.cmake) calls, declared in libbb.h. vi.c itself is never
 * patched -- everything it expects from "libbb" is supplied from here
 * instead, per libbb.h's own top comment.
 *
 * Two kinds of helper live in this file:
 *  - Plain C helpers (xmalloc/xzalloc/xrealloc/xstrdup/xstrndup/xasprintf/
 *    bb_strtou/skip_whitespace/bb_putchar/fflush_all/full_read/full_write/
 *    safe_read/bb_show_usage/bb_simple_error_msg_and_die) that need
 *    nothing BruceOS-specific: malloc/free/printf/read/write/exit are
 *    already real, resolvable symbols for any Bruce ELF app (see
 *    elf_loader_sdk_symbols.c's libc-alias block), so these are written
 *    exactly like any portable libc helper would be.
 *  - Terminal-facing helpers (safe_read_key, safe_poll, set_termios_to_raw,
 *    tcsetattr_stdin_TCSANOW, get_terminal_width_height) that route
 *    through core_sdk/stdio.h + core_sdk/tty.h instead of a real termios
 *    driver/ioctl -- this app's actual point of contact with BruceOS.
 *
 * Peek/consume note: vi.c only ever reaches stdin through two paths --
 * mysleep()'s safe_poll() ("is a key already waiting, without consuming
 * it?") and readit()'s safe_read_key() ("block until a key arrives, and
 * decode it"). stdio__read() has no non-consuming peek mode, so both
 * paths share one static one-byte pushback slot below: a safe_poll() that
 * finds a byte stashes it there instead of discarding it, and the next
 * safe_read_key() (or a later safe_poll()) drains that slot first. This
 * is safe *only* because grepping the fetched vi.c confirms these two are
 * its sole stdin entry points -- no bare read(STDIN_FILENO, ...) exists
 * anywhere else in it that could race past this slot. */

#include "libbb.h"

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"

const char *applet_name = "vi";

/* Storage for libbb.h's `extern struct globals *ptr_to_globals;` -- see
 * that declaration's own comment. Untyped (void*-compatible pointer to an
 * incomplete type) here; vi.c's G macro casts every access against its
 * own local "struct globals" definition. */
struct globals *ptr_to_globals;

/* ---- plain helpers -------------------------------------------------- */

int bb_putchar(int ch) {
    char c = (char)ch;
    stdio__write(&c, 1);
    return ch;
}

void bb_show_usage(void) {
    stdio__printf("Usage: vi [-h] [-c CMD] [-R] [-H] [FILE]...\n");
    exit(1);
}

void bb_simple_error_msg_and_die(const char *s) {
    stdio__printf("vi: %s\n", s ? s : "");
    exit(1);
}

unsigned bb_strtou(const char *arg, char **endp, int base) {
    if (arg == NULL) {
        errno = EINVAL;
        return (unsigned)-1;
    }
    char *end;
    errno = 0;
    unsigned long value = strtoul(arg, &end, base);
    if (end == arg || (endp == NULL && *end != '\0')) errno = EINVAL;
    if (endp != NULL) *endp = end;
    if (errno != 0) return (unsigned)-1;
    return (unsigned)value;
}

int fflush_all(void) {
    /* stdio__write() is unbuffered at this layer (a thin wrapper over the
     * calling process's routed session) -- nothing to flush. */
    return 0;
}

ssize_t full_read(int fd, void *buf, size_t count) {
    char *p = (char *)buf;
    size_t done = 0;
    while (done < count) {
        ssize_t n = read(fd, p + done, count - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return done != 0 ? (ssize_t)done : -1;
        }
        if (n == 0) break; /* EOF */
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t full_write(int fd, const void *buf, size_t count) {
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < count) {
        ssize_t n = write(fd, p + done, count - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return done != 0 ? (ssize_t)done : -1;
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t safe_read(int fd, void *buf, size_t count) {
    ssize_t n;
    do {
        n = read(fd, buf, count);
    } while (n < 0 && errno == EINTR);
    return n;
}

char *skip_whitespace(const char *s) {
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    return (char *)s;
}

char *skip_non_whitespace(const char *s) {
    while (*s != '\0' && !isspace((unsigned char)*s)) s++;
    return (char *)s;
}

char *concat_path_file(const char *path, const char *filename) {
    if (path == NULL) path = "";
    size_t path_len = strlen(path);
    bool has_trailing_slash = path_len > 0 && path[path_len - 1] == '/';
    while (*filename == '/') filename++;
    return xasprintf("%s%s%s", path, has_trailing_slash ? "" : "/", filename);
}

/* `strings` is a sequence of NUL-terminated substrings, the whole thing
 * terminated by an extra NUL (a "\0"-separated string table) -- returns
 * the 0-based index of the one equal to `key`, or -1. */
int index_in_strings(const char *strings, const char *key) {
    int index = 0;
    while (*strings != '\0') {
        size_t len = strlen(strings);
        if (strcmp(strings, key) == 0) return index;
        strings += len + 1;
        index++;
    }
    return -1;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = n; i > 0; i--) {
        if (p[i - 1] == (unsigned char)c) return (void *)(p + i - 1);
    }
    return NULL;
}

int fputs_stdout(const char *s) {
    if (s == NULL) return 0;
    return stdio__write(s, strlen(s)) == BRUCE_OK ? 0 : EOF;
}

/* ---- llist_t / getopt32 ------------------------------------------------
 * Both exist only to support vi.c's one getopt32() call site (VI_OPTSTR,
 * see vi.c -- "Hh" bare flags, "R" bare flag, "c:*" a repeatable
 * argument-taking option collected into an llist_t*) -- see libbb.h's
 * getopt32() comment for why this is a purpose-built subset rather than
 * general BusyBox getopt32()'s full option-string grammar. */

struct llist_t {
    llist_t *link;
    char *data;
};

char *llist_pop(llist_t **head) {
    llist_t *node = *head;
    if (node == NULL) return NULL;
    char *data = node->data;
    *head = node->link;
    free(node);
    return data;
}

/* Appends (not prepends) so "-c cmd1 -c cmd2" runs in the order given --
 * vi.c drains this list front-to-back (while (initial_cmds)
 * run_cmds(llist_pop(&initial_cmds));), so list order is command order. */
static void vi_llist_append(llist_t **head, char *data) {
    llist_t *node = xmalloc(sizeof(*node));
    node->data = data;
    node->link = NULL;
    if (*head == NULL) {
        *head = node;
        return;
    }
    llist_t *tail = *head;
    while (tail->link != NULL) tail = tail->link;
    tail->link = node;
}

int optind = 1;

uint32_t getopt32(char **argv, const char *optstring, ...) {
    struct {
        char letter;
        bool has_arg;
        bool is_list;
    } opts[32];
    int option_count = 0;
    for (const char *p = optstring; *p != '\0' && option_count < 32;) {
        char letter = *p++;
        bool has_arg = false, is_list = false;
        if (*p == ':') {
            has_arg = true;
            p++;
            if (*p == '*') {
                is_list = true;
                p++;
            }
        }
        opts[option_count].letter = letter;
        opts[option_count].has_arg = has_arg;
        opts[option_count].is_list = is_list;
        option_count++;
    }

    /* One llist_t** per has_arg option, in optstring order -- matches
     * this call site's single "c:*" option exactly; a scalar (non-list)
     * arg-taking option isn't needed by vi.c and isn't handled here. */
    llist_t **list_storage[32];
    va_list args;
    va_start(args, optstring);
    for (int i = 0; i < option_count; i++) {
        list_storage[i] = opts[i].has_arg ? va_arg(args, llist_t **) : NULL;
    }
    va_end(args);

    uint32_t result = 0;
    int i = 1;
    for (; argv[i] != NULL; i++) {
        char *arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0') break; /* first non-option arg */
        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        for (int j = 1; arg[j] != '\0'; j++) {
            int idx = -1;
            for (int k = 0; k < option_count; k++) {
                if (opts[k].letter == arg[j]) {
                    idx = k;
                    break;
                }
            }
            if (idx < 0) continue; /* unrecognized flag -- ignored */
            result |= (1u << idx);
            if (opts[idx].has_arg) {
                char *value;
                if (arg[j + 1] != '\0') {
                    value = &arg[j + 1]; /* "-cVALUE" */
                } else if (argv[i + 1] != NULL) {
                    value = argv[++i]; /* "-c VALUE" */
                } else {
                    value = (char *)"";
                }
                if (opts[idx].is_list && list_storage[idx] != NULL) {
                    vi_llist_append(list_storage[idx], value);
                }
                break; /* an arg-taking flag consumes the rest of this token */
            }
        }
    }
    optind = i;
    return result;
}

void *xmalloc(size_t size) {
    void *p = malloc(size != 0 ? size : 1);
    if (p == NULL) bb_simple_error_msg_and_die("out of memory");
    return p;
}

void *xzalloc(size_t size) {
    void *p = xmalloc(size);
    memset(p, 0, size);
    return p;
}

void *xrealloc(void *old, size_t size) {
    void *p = realloc(old, size != 0 ? size : 1);
    if (p == NULL) bb_simple_error_msg_and_die("out of memory");
    return p;
}

char *xstrdup(const char *s) {
    if (s == NULL) return xzalloc(1);
    size_t len = strlen(s) + 1;
    char *copy = xmalloc(len);
    memcpy(copy, s, len);
    return copy;
}

char *xstrndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len] != '\0') len++;
    char *copy = xmalloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *xasprintf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    if (needed < 0) bb_simple_error_msg_and_die("xasprintf failed");
    char *buffer = xmalloc((size_t)needed + 1);
    vsnprintf(buffer, (size_t)needed + 1, format, args);
    va_end(args);
    return buffer;
}

void *xmalloc_open_read_close(const char *filename, size_t *maxsz_p) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    size_t cap = 4096;
    if (fstat(fd, &st) == 0 && st.st_size > 0) cap = (size_t)st.st_size;
    if (maxsz_p != NULL && *maxsz_p != 0 && *maxsz_p < cap) cap = *maxsz_p;

    char *buf = xmalloc(cap + 1);
    size_t size = 0;
    for (;;) {
        if (size == cap) {
            cap += cap / 2 + 4096;
            if (maxsz_p != NULL && *maxsz_p != 0 && cap > *maxsz_p) cap = *maxsz_p;
            buf = xrealloc(buf, cap + 1);
        }
        ssize_t n = full_read(fd, buf + size, cap - size);
        if (n <= 0) break;
        size += (size_t)n;
        if (maxsz_p != NULL && *maxsz_p != 0 && size >= *maxsz_p) break;
    }
    close(fd);

    buf = xrealloc(buf, size + 1);
    buf[size] = '\0';
    if (maxsz_p != NULL) *maxsz_p = size;
    return buf;
}

/* ---- terminal-facing helpers ----------------------------------------- */

/* Shared pushback slot -- see this file's top comment. */
static bool s_stdin_pending;
static uint8_t s_stdin_pending_byte;

/* Fills the pushback slot from stdio__read() if it's empty, waiting up to
 * timeout_ms (0 = poll, UINT32_MAX = block forever, matching
 * stdio__read()'s own contract). Returns true if a byte is available
 * (already was, or just arrived) without consuming it. */
static bool vi_stdin_peek(uint32_t timeout_ms) {
    if (s_stdin_pending) return true;
    uint8_t byte;
    size_t received = 0;
    if (stdio__read(&byte, 1, timeout_ms, &received) == BRUCE_OK && received == 1) {
        s_stdin_pending_byte = byte;
        s_stdin_pending = true;
        return true;
    }
    return false;
}

/* Drains the pushback slot (peeking first with the same timeout) into
 * *out. Returns true on success. */
static bool vi_stdin_getbyte(uint32_t timeout_ms, uint8_t *out) {
    if (!vi_stdin_peek(timeout_ms)) return false;
    *out = s_stdin_pending_byte;
    s_stdin_pending = false;
    return true;
}

int safe_poll(struct pollfd *ufds, unsigned nfds, int timeout) {
    /* vi.c only ever calls this via mysleep() with a single stdin pollfd
     * (grepped) -- ufds/nfds are accepted for signature compatibility but
     * not otherwise inspected. timeout is milliseconds, -1 for "block
     * forever", matching poll(2)'s own contract (and stdio__read()'s
     * UINT32_MAX)); mysleep(0) (poll, don't block) is vi.c's main-loop
     * "is there a key already waiting" check. */
    (void)ufds;
    (void)nfds;
    uint32_t timeout_ms = timeout < 0 ? UINT32_MAX : (uint32_t)timeout;
    return vi_stdin_peek(timeout_ms) ? 1 : 0;
}

/* Decodes the small set of arrow/home/end/insert/delete/page-up/page-down
 * VT100-ish escape sequences vi.c's do_cmd() switches on (see libbb.h's
 * KEYCODE_* comment for exactly which ones and why only those). After a
 * lone ESC, waits briefly (50ms -- long enough for a real escape sequence,
 * whose bytes arrive back-to-back, short enough that a standalone Escape
 * keypress doesn't feel laggy) for a continuation before deciding it was
 * just Escape by itself. */
static int64_t vi_decode_escape(void) {
    uint8_t c1;
    if (!vi_stdin_getbyte(50, &c1)) return 27; /* bare ESC */

    if (c1 == '[' || c1 == 'O') {
        uint8_t c2;
        if (!vi_stdin_getbyte(50, &c2)) return 27;
        switch (c2) {
            case 'A': return KEYCODE_UP;
            case 'B': return KEYCODE_DOWN;
            case 'C': return KEYCODE_RIGHT;
            case 'D': return KEYCODE_LEFT;
            case 'H': return KEYCODE_HOME;
            case 'F': return KEYCODE_END;
            default: break;
        }
        if (c1 == '[' && c2 >= '1' && c2 <= '9') {
            /* "ESC [ <digit> ~" -- insert/delete/page up/page down. Only
             * a single digit is expected from any of these codes, so the
             * closing '~' is consumed and checked, but not accumulated
             * into a multi-digit number (vi.c has no keycode beyond this
             * set that this port defines, see libbb.h). */
            uint8_t c3;
            if (!vi_stdin_getbyte(50, &c3)) return 27;
            if (c3 == '~') {
                switch (c2) {
                    case '2': return KEYCODE_INSERT;
                    case '3': return KEYCODE_DELETE;
                    case '5': return KEYCODE_PAGEUP;
                    case '6': return KEYCODE_PAGEDOWN;
                    default: break;
                }
            }
        }
        return 27; /* unrecognized sequence -- report plain ESC */
    }

    return 27;
}

int64_t safe_read_key(int fd, char *buffer, int timeout) {
    (void)fd;   /* always STDIN_FILENO in this app -- see caller */
    (void)buffer; /* upstream's own pushback scratch space; unused here, see this file's top comment */
    uint32_t timeout_ms = timeout < 0 ? UINT32_MAX : (uint32_t)timeout;

    uint8_t c;
    if (!vi_stdin_getbyte(timeout_ms, &c)) {
        errno = 0; /* EOF, not an error -- matches read_key()'s own contract */
        return -1;
    }
    if (c == 0x1b) return vi_decode_escape();
    return (int64_t)c;
}

/* No real termios driver underneath a Bruce ELF app's stdin (see
 * core_sdk/tty.h's own doc comment: raw mode is already the default
 * behavior for any reader, and "cooked" vs "raw" is cooperative
 * bookkeeping, not a line-discipline switch) -- so unlike a real
 * tcgetattr()/tcsetattr() pair, there's no actual terminal state to save
 * into *oldterm before switching. It's still zero-filled and handed back,
 * since vi.c dereferences it later (isbackspace()'s term_orig.c_cc[VERASE]
 * read, see libbb.h) -- zero there is the correct "no such key" answer. */
int set_termios_to_raw(int fd, struct termios *oldterm, int flags) {
    (void)fd;
    (void)flags; /* TERMIOS_RAW_CRNL vs other flag combos -- moot, see above */
    if (oldterm != NULL) memset(oldterm, 0, sizeof(*oldterm));
    tty__set_mode(BRUCE_TTY_MODE_RAW);
    return 0;
}

int tcsetattr_stdin_TCSANOW(const struct termios *tp) {
    (void)tp; /* nothing in it to apply -- see set_termios_to_raw()'s comment */
    tty__set_mode(BRUCE_TTY_MODE_COOKED);
    return 0;
}

int get_terminal_width_height(int fd, unsigned *width, unsigned *height) {
    (void)fd; /* always STDIN_FILENO here */
    bruce_tty_size_t size;
    if (tty__get_size(&size) != BRUCE_OK) return -1;
    if (width != NULL) *width = size.columns;
    if (height != NULL) *height = size.rows;
    return 0;
}
