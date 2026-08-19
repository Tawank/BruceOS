#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"

/*
 * Pager command: less.
 *
 * Unlike the "text" editor/viewer (modules/text/text_app.c), which claims
 * the *physical* display and input directly, this renders entirely through
 * ANSI/VT100 escape sequences written to the calling process's own routed
 * stdio session -- the same way `cat`, `ssh`, and the shell's own "clear"
 * builtin do. Whatever is actually driving that session (terminal_app.c's
 * cell-grid emulator in modules/utils/terminal/terminal_ansi.c, an ssh
 * client, a serial console) renders it, so this file never touches
 * core_sdk/display.h or core_sdk/input.h. See terminal_ansi.h's own comment
 * for the exact escape-sequence subset a client can rely on -- this uses
 * only cursor positioning (CSI H), erase display (CSI 2J), reverse video
 * (CSI 7m/0m), and cursor visibility (CSI ?25h/l), all supported there.
 */

/* Generous now that content is memory__external_alloc()-backed (PSRAM, or
 * swap, or plain internal RAM as a last resort -- see memory__external_alloc()
 * in core_sdk/memory.h) rather than a plain memory__malloc() buffer capped by
 * the internal heap's largest free block. */
#define LESS_MAX_BYTES (512u * 1024u)
#define LESS_ESCAPE_TIMEOUT_MS 50u
#define LESS_ESCAPE_MAX 8u
/* How often the input read wakes up with nothing typed, purely so a tty
 * resize (see core_sdk/tty.h's generation counter -- there's no SIGWINCH
 * equivalent) gets noticed without a key being pressed. */
#define LESS_POLL_TIMEOUT_MS 250u
#define LESS_SEARCH_MAX 96u
#define LESS_HSCROLL_STEP 8u
#define LESS_DEFAULT_COLUMNS 80u
#define LESS_DEFAULT_ROWS 24u

typedef struct {
    const char *data;
    size_t length;
    size_t top;         /* byte offset of the first visible line (always a line start) */
    size_t left_column;
    uint16_t columns;
    uint16_t rows; /* total rows including the status line */
    char path[BRUCE_STORAGE_PATH_MAX];
    char *line_buffer; /* columns + 1 bytes, reused for both content rows and the status bar */
    char search[LESS_SEARCH_MAX];
} less__state_t;

typedef enum {
    LESS_ACTION_NONE = 0,
    LESS_ACTION_QUIT,
    LESS_ACTION_LINE_DOWN,
    LESS_ACTION_LINE_UP,
    LESS_ACTION_PAGE_DOWN,
    LESS_ACTION_PAGE_UP,
    LESS_ACTION_TOP,
    LESS_ACTION_BOTTOM,
    LESS_ACTION_LEFT,
    LESS_ACTION_RIGHT,
    LESS_ACTION_SEARCH,
    LESS_ACTION_SEARCH_NEXT,
} less__action_t;

/* -------------------------------------------------------------------------
 * Line-offset navigation. No line index is precomputed -- every move is a
 * bounded scan of at most a screenful of lines, which is cheap enough to
 * redo on every keypress and avoids an O(lines) side table for content that
 * can be hundreds of KiB.
 * ------------------------------------------------------------------------- */

static size_t less__line_start(const char *data, size_t pos) {
    while (pos > 0 && data[pos - 1] != '\n') pos--;
    return pos;
}

static size_t less__next_line(const char *data, size_t length, size_t offset) {
    while (offset < length && data[offset] != '\n') offset++;
    return offset < length ? offset + 1u : length;
}

static size_t less__forward_lines(const char *data, size_t length, size_t offset, size_t n) {
    for (size_t i = 0; i < n && offset < length; ++i) offset = less__next_line(data, length, offset);
    return offset;
}

static size_t less__back_lines(const char *data, size_t line_start_offset, size_t n) {
    size_t offset = line_start_offset;
    for (size_t i = 0; i < n && offset > 0; ++i) offset = less__line_start(data, offset - 1u);
    return offset;
}

static uint16_t less__visible_rows(uint16_t rows) { return rows > 1u ? (uint16_t)(rows - 1u) : rows; }

/* The top offset that puts the file's last line at the bottom of a full
 * page, i.e. what 'G' (and page-down clamping) scrolls to. */
static size_t less__last_page_top(const char *data, size_t length, size_t visible_rows) {
    size_t last_line = less__line_start(data, length);
    return less__back_lines(data, last_line, visible_rows > 0 ? visible_rows - 1u : 0u);
}

static bool less__scroll_down(less__state_t *state, size_t lines, size_t last_top) {
    size_t next = less__forward_lines(state->data, state->length, state->top, lines);
    if (next > last_top) next = last_top;
    if (next == state->top) return false;
    state->top = next;
    return true;
}

static bool less__scroll_up(less__state_t *state, size_t lines) {
    size_t prev = less__back_lines(state->data, state->top, lines);
    if (prev == state->top) return false;
    state->top = prev;
    return true;
}

static bool
less__find(const char *data, size_t length, size_t from, const char *needle, size_t needle_len, size_t *out_offset) {
    if (needle_len == 0 || from > length || needle_len > length - from) return false;
    for (size_t i = from; i + needle_len <= length; ++i) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            *out_offset = less__line_start(data, i);
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */

/* Renders at most `out_capacity` visible columns of a source line into
 * `out`: a tab becomes two spaces, and every other C0 control byte or DEL
 * becomes a single visible placeholder, so control bytes can't reach the
 * terminal raw. Real less(1) does the same by default (caret notation)
 * rather than writing them straight through: arbitrary piped/curled content
 * -- HTML, a script, plain binary -- can otherwise embed ESC sequences the
 * terminal emulator faithfully executes as real cursor moves or SGR/color
 * changes, which is what garbled the screen and left a color bleeding into
 * the reverse-video status bar depending on which lines happened to be on
 * screen. Column-budgeted (rather than a 1:1 byte copy) because a tab's
 * two-space expansion means the number of source bytes consumed and the
 * number of columns produced aren't the same -- returns the latter. */
static size_t less__sanitize(char *out, size_t out_capacity, const char *in, size_t length) {
    size_t written = 0;
    for (size_t i = 0; i < length && written < out_capacity; ++i) {
        unsigned char byte = (unsigned char)in[i];
        if (byte == '\t') {
            out[written++] = ' ';
            if (written < out_capacity) out[written++] = ' ';
        } else {
            out[written++] = (byte < 0x20 || byte == 0x7f) ? '.' : (char)byte;
        }
    }
    return written;
}

static void less__draw(less__state_t *state, const char *message) {
    /* Hide cursor, erase the screen, home the cursor, and reset any SGR
     * state left over from the previous draw (erase-display alone doesn't
     * reset colors/attributes -- see less__sanitize()'s comment on why that
     * otherwise showed up as a status bar with drifting colors). The old
     * length here (10) silently truncated this string before the "home
     * cursor" (\033[H) sequence, so every redraw started wherever the
     * cursor last happened to be instead of the top-left corner. */
    stdio__write("\033[?25l\033[2J\033[H\033[0m", 17);
    uint16_t visible_rows = less__visible_rows(state->rows);
    size_t offset = state->top;
    for (uint16_t row = 0; row < visible_rows; ++row) {
        size_t line_end = offset;
        while (line_end < state->length && state->data[line_end] != '\n') line_end++;
        size_t line_len = line_end - offset;
        size_t start = offset + (state->left_column < line_len ? state->left_column : line_len);
        size_t written =
            less__sanitize(state->line_buffer, state->columns, state->data + start, line_end - start);
        (void)stdio__write(state->line_buffer, written);
        if ((uint16_t)(row + 1u) < visible_rows) (void)stdio__write("\r\n", 2);
        offset = line_end < state->length ? line_end + 1u : state->length;
    }
    if (visible_rows >= state->rows) return; /* no room for a status line */

    char *status = state->line_buffer;
    unsigned percent = state->length == 0 ? 100u : (unsigned)((uint64_t)state->top * 100u / state->length);
    int written = message != NULL
                      ? snprintf(status, (size_t)state->columns + 1u, "%s", message)
                      : snprintf(
                            status, (size_t)state->columns + 1u, "%s (%u%%) -- q:quit space:next b:prev /:find n:next",
                            state->path, percent
                        );
    size_t text_len = written < 0 ? 0 : (size_t)written;
    if (text_len > state->columns) text_len = state->columns;
    memset(status + text_len, ' ', state->columns - text_len);
    (void)stdio__write("\r\n\033[7m", 6);
    (void)stdio__write(status, state->columns);
    (void)stdio__write("\033[0m", 4);
}

/* -------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */

/* A byte 0-255 on success, or a negative bruce_result_t (BRUCE_ERR_TIMEOUT
 * when nothing arrived within `timeout_ms`, BRUCE_ERR_CANCELLED on Ctrl+C/
 * process termination, ...). */
static int less__read_byte(uint32_t timeout_ms) {
    unsigned char byte;
    size_t size = 0;
    bruce_result_t result = stdio__read(&byte, 1, timeout_ms, &size);
    return result == BRUCE_OK && size == 1 ? (int)byte : (int)result;
}

/* Follows an ESC already consumed from the input stream. Mirrors
 * shell_console__read_escape()'s approach: the rest of a CSI sequence
 * arrives essentially atomically (see terminal_app.c's terminal__write_input,
 * which forwards a whole arrow/page key as one write), so a short per-byte
 * timeout is enough to tell "more sequence coming" from "just ESC". */
static size_t less__read_escape(unsigned char *sequence, size_t capacity) {
    size_t used = 0;
    while (used < capacity) {
        int input = less__read_byte(LESS_ESCAPE_TIMEOUT_MS);
        if (input < 0) break;
        unsigned char byte = (unsigned char)input;
        sequence[used++] = byte;
        if (used > 1 && byte >= 0x40 && byte <= 0x7e) break;
    }
    return used;
}

static less__action_t less__decode_escape(void) {
    unsigned char sequence[LESS_ESCAPE_MAX] = {0};
    size_t size = less__read_escape(sequence, sizeof(sequence));
    if (size < 2 || sequence[0] != '[') return LESS_ACTION_QUIT; /* bare ESC: quit, like most pagers */
    switch (sequence[size - 1]) {
        case 'A': return LESS_ACTION_LINE_UP;
        case 'B': return LESS_ACTION_LINE_DOWN;
        case 'C': return LESS_ACTION_RIGHT;
        case 'D': return LESS_ACTION_LEFT;
        case 'H': return LESS_ACTION_TOP;
        case '~':
            if (size == 3 && sequence[1] == '5') return LESS_ACTION_PAGE_UP;
            if (size == 3 && sequence[1] == '6') return LESS_ACTION_PAGE_DOWN;
            return LESS_ACTION_NONE;
        default: return LESS_ACTION_NONE;
    }
}

static less__action_t less__decode_byte(int input) {
    switch (input) {
        case 'q':
        case 0x03: return LESS_ACTION_QUIT; /* Ctrl+C */
        case ' ':
        case 'f':
        case 0x06: return LESS_ACTION_PAGE_DOWN; /* Ctrl+F */
        case 'b':
        case 0x02: return LESS_ACTION_PAGE_UP; /* Ctrl+B */
        case 'j':
        case '\n':
        case '\r': return LESS_ACTION_LINE_DOWN;
        case 'k': return LESS_ACTION_LINE_UP;
        case 'g': return LESS_ACTION_TOP;
        case 'G': return LESS_ACTION_BOTTOM;
        case '/': return LESS_ACTION_SEARCH;
        case 'n': return LESS_ACTION_SEARCH_NEXT;
        default: return LESS_ACTION_NONE;
    }
}

/* Draws a plain (non-reverse) "/" prompt on the status line and line-edits
 * into `state->search` until Enter. Esc, Ctrl+C, or an empty pattern cancel
 * without changing `state->search`. */
static bool less__prompt_search(less__state_t *state) {
    char buffer[LESS_SEARCH_MAX] = {0};
    size_t length = 0;
    char position[16];
    snprintf(position, sizeof(position), "\033[%u;1H\033[K", (unsigned)state->rows);
    for (;;) {
        (void)stdio__write(position, strlen(position));
        (void)stdio__write("/", 1);
        (void)stdio__write(buffer, length);
        int input = less__read_byte(UINT32_MAX);
        if (input < 0 || input == 0x1b || input == 0x03) return false;
        if (input == '\n' || input == '\r') break;
        if (input == '\b' || input == 0x7f) {
            if (length > 0) length--;
            continue;
        }
        if (input >= 0x20 && input <= 0x7e && length + 1u < sizeof(buffer) &&
            length + 1u < (size_t)state->columns) {
            buffer[length++] = (char)input;
        }
    }
    if (length == 0) return false;
    buffer[length] = '\0';
    snprintf(state->search, sizeof(state->search), "%s", buffer);
    return true;
}

static void less__search(less__state_t *state, const char **message) {
    size_t from = less__forward_lines(state->data, state->length, state->top, 1);
    size_t found = 0;
    if (less__find(state->data, state->length, from, state->search, strlen(state->search), &found)) {
        state->top = found;
    } else {
        *message = "Pattern not found";
    }
}

static void less__run(less__state_t *state) {
    bool running = true;
    bool dirty = true;
    const char *message = NULL;
    uint32_t last_generation = 0;
    /* Belt and suspenders alongside less__sanitize()'s per-line clamp to
     * state->columns: disable the terminal's own autowrap (DECAWM, mode 7 --
     * supported by terminal_ansi.c) for as long as this pager owns the
     * screen, so a line this pager means to chop can never instead get
     * wrapped by the terminal itself onto the row below, which is what
     * produced the "line is cut and continues below" look. Restored below on
     * exit -- the shell (and everything after it) expects autowrap on. */
    (void)stdio__write("\033[?7l", 5);
    while (running) {
        bruce_tty_size_t size;
        if (tty__get_size(&size) == BRUCE_OK && size.generation != last_generation) {
            last_generation = size.generation;
            if (size.columns > 0 && size.columns != state->columns) {
                char *grown = memory__realloc(state->line_buffer, (size_t)size.columns + 1u);
                if (grown != NULL) {
                    state->line_buffer = grown;
                    state->columns = size.columns;
                }
            }
            if (size.rows > 0) state->rows = size.rows;
            dirty = true;
        }
        if (dirty) {
            less__draw(state, message);
            message = NULL;
            dirty = false;
        }

        int input = less__read_byte(LESS_POLL_TIMEOUT_MS);
        if (input == BRUCE_ERR_TIMEOUT) continue;
        if (input < 0) break; /* cancelled (Ctrl+C at the process level) or a broken session */

        less__action_t action = input == 0x1b ? less__decode_escape() : less__decode_byte(input);
        uint16_t visible_rows = less__visible_rows(state->rows);
        size_t last_top = less__last_page_top(state->data, state->length, visible_rows);
        switch (action) {
            case LESS_ACTION_QUIT: running = false; break;
            case LESS_ACTION_LINE_DOWN: dirty = less__scroll_down(state, 1, last_top) || dirty; break;
            case LESS_ACTION_LINE_UP: dirty = less__scroll_up(state, 1) || dirty; break;
            case LESS_ACTION_PAGE_DOWN: dirty = less__scroll_down(state, visible_rows, last_top) || dirty; break;
            case LESS_ACTION_PAGE_UP: dirty = less__scroll_up(state, visible_rows) || dirty; break;
            case LESS_ACTION_TOP:
                dirty = state->top != 0 || state->left_column != 0 || dirty;
                state->top = 0;
                state->left_column = 0;
                break;
            case LESS_ACTION_BOTTOM: dirty = (state->top != last_top) || dirty; state->top = last_top; break;
            case LESS_ACTION_LEFT:
                if (state->left_column > 0) {
                    size_t step = state->left_column < LESS_HSCROLL_STEP ? state->left_column : LESS_HSCROLL_STEP;
                    state->left_column -= step;
                    dirty = true;
                }
                break;
            case LESS_ACTION_RIGHT: state->left_column += LESS_HSCROLL_STEP; dirty = true; break;
            case LESS_ACTION_SEARCH:
                if (less__prompt_search(state)) less__search(state, &message);
                dirty = true;
                break;
            case LESS_ACTION_SEARCH_NEXT:
                if (state->search[0] == '\0') message = "No previous pattern";
                else less__search(state, &message);
                dirty = true;
                break;
            default: break;
        }
    }
    /* Restore autowrap (see the \033[?7l comment above), show the cursor,
     * clear the screen, home the cursor, and reset SGR before handing the
     * terminal back. The old length here (14) was also short by 3 bytes,
     * silently truncating the trailing "m" off "\033[0m". */
    (void)stdio__write("\033[?7h\033[?25h\033[2J\033[H\033[0m", 22);
}

/* -------------------------------------------------------------------------
 * Content loading
 * ------------------------------------------------------------------------- */

static bruce_result_t less__load_path(const char *path, bruce_memory_object_t *object, size_t *out_length) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    uint64_t size = 0;
    result = storage__seek(file, 0, SEEK_END, &size);
    if (result == BRUCE_OK && size > LESS_MAX_BYTES) result = BRUCE_ERR_RESOURCE_LIMIT;
    if (result == BRUCE_OK) result = storage__seek(file, 0, SEEK_SET, NULL);

    bruce_memory_object_t obj = {0};
    if (result == BRUCE_OK && size > 0) result = memory__external_alloc((size_t)size, &obj);
    size_t offset = 0;
    unsigned char chunk[256];
    while (result == BRUCE_OK && offset < (size_t)size) {
        size_t read_size = 0;
        result = storage__read(file, chunk, sizeof(chunk), &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) result = memory__external_write(&obj, offset, chunk, read_size);
        offset += read_size;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        if (obj.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&obj);
        return result;
    }
    *object = obj;
    *out_length = offset;
    return BRUCE_OK;
}

static bruce_result_t less__load_stdin(size_t size, bruce_memory_object_t *object, size_t *out_length) {
    bruce_memory_object_t obj = {0};
    bruce_result_t result = size > 0 ? memory__external_alloc(size, &obj) : BRUCE_OK;
    size_t offset = 0;
    unsigned char chunk[256];
    while (result == BRUCE_OK && offset < size) {
        size_t want = size - offset > sizeof(chunk) ? sizeof(chunk) : size - offset;
        size_t read_size = 0;
        result = stdio__read(chunk, want, UINT32_MAX, &read_size);
        if (result == BRUCE_OK && read_size == 0) result = BRUCE_ERR_IO;
        if (result == BRUCE_OK) result = memory__external_write(&obj, offset, chunk, read_size);
        offset += read_size;
    }
    if (result != BRUCE_OK) {
        if (obj.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&obj);
        return result;
    }
    *object = obj;
    *out_length = offset;
    return BRUCE_OK;
}

int bnu_less_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("View a file, or piped stdin, one screen at a time.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_str_opt(parser, "stdin-size", NULL);
    ap_set_opt_help(parser, "stdin-size", "Read exactly this many bytes from stdin (used by shell pipes)");
    ap_add_optional_arg(parser, "path", "File to view");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *path_arg = ap_get_arg(parser, "path");
    const char *stdin_size_arg = ap_found(parser, "stdin-size") ? ap_get_str_value(parser, "stdin-size") : NULL;
    char *end = NULL;
    unsigned long parsed_stdin_size = stdin_size_arg != NULL ? strtoul(stdin_size_arg, &end, 10) : 0;
    bool stdin_requested = stdin_size_arg != NULL;
    bool from_stdin = stdin_requested && stdin_size_arg[0] != '\0' && end != NULL && *end == '\0' &&
                      parsed_stdin_size <= LESS_MAX_BYTES;
    char path[BRUCE_STORAGE_PATH_MAX] = {0};
    bool path_resolved = !from_stdin && path_arg != NULL && bnu__resolve_path(path_arg, path);
    ap_free(parser);

    if (stdin_requested && !from_stdin) {
        stdio__printf("less: invalid --stdin-size\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!from_stdin && path_arg == NULL) {
        stdio__printf("less: missing filename\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!from_stdin && !path_resolved) return BRUCE_ERR_INVALID_PATH;

    bruce_memory_object_t object = {0};
    size_t length = 0;
    bruce_result_t result = from_stdin ? less__load_stdin((size_t)parsed_stdin_size, &object, &length)
                                       : less__load_path(path, &object, &length);
    if (result != BRUCE_OK) {
        stdio__printf("less: %s: %s\n", from_stdin ? "(stdin)" : path, result__to_string(result));
        return result;
    }

    const void *data = NULL;
    if (length > 0 && memory__external_map(&object, &data) != BRUCE_OK) {
        if (object.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&object);
        return BRUCE_ERR_IO;
    }

    if (!tty__isatty()) {
        /* No interactive terminal to page against -- fall back to a plain
         * dump, matching real less(1) when its output isn't a tty. */
        if (length > 0) (void)stdio__write(data, length);
        if (object.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&object);
        return BRUCE_OK;
    }

    bruce_tty_size_t tty_size = {.columns = LESS_DEFAULT_COLUMNS, .rows = LESS_DEFAULT_ROWS};
    (void)tty__get_size(&tty_size);
    if (tty_size.columns == 0) tty_size.columns = LESS_DEFAULT_COLUMNS;
    if (tty_size.rows == 0) tty_size.rows = LESS_DEFAULT_ROWS;

    less__state_t state = {
        .data = data != NULL ? data : "",
        .length = length,
        .columns = tty_size.columns,
        .rows = tty_size.rows,
    };
    snprintf(state.path, sizeof(state.path), "%s", from_stdin ? "(stdin)" : path);
    state.line_buffer = memory__malloc((size_t)state.columns + 1u);
    if (state.line_buffer != NULL) {
        less__run(&state);
        memory__free(state.line_buffer);
    } else {
        result = BRUCE_ERR_NO_MEMORY;
    }
    if (object.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&object);
    return result;
}
