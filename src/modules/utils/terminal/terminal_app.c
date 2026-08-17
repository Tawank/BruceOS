#include "terminal_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"
#include "terminal_ansi.h"

#define TERMINAL__LINE_CAPACITY 512
/* Mirrors the active font's cell size (display__get_font_metrics()) as
 * compile-time constants -- every built-in font is monospace 6x10 today. */
#define TERMINAL__CHAR_W 6
#define TERMINAL__CHAR_H 10
#define TERMINAL__TITLE_H 12
#define TERMINAL__FRAME_MARGIN 2
/* Generous upper bound on grid size, purely to keep the cell-buffer
 * allocation bounded; no real board's display comes close to this. */
#define TERMINAL__MAX_COLUMNS 200
#define TERMINAL__MAX_ROWS 80
#define TERMINAL__INPUT_TIMEOUT_MS 50
#define TERMINAL__HIDDEN_DELAY_MS 20
#define TERMINAL__CHILD_STOP_TIMEOUT_MS 500
#define TERMINAL__ASCII_ESCAPE 0x1b
#define TERMINAL__ASCII_DELETE 0x7f

typedef struct {
    terminal_cell_t *cells;
    terminal_cell_t *alt_cells;
    bruce_memory_object_t cells_object;
    bruce_memory_object_t alt_cells_object;
    bool cells_external;
    bool alt_cells_external;
    terminal_grid_t grid;
    bruce_stdio_session_t session;
    bruce_process_id_t child;
    bool dirty;
    bool exit_requested;
} terminal__state_t;

/* Prefers a PSRAM- or plain-internal-RAM-backed memory__external block (both
 * are plain, directly addressable buffers) so the grid can keep mutating it
 * in place with ordinary pointer writes. Falls back to a separate, untracked
 * internal heap allocation when PSRAM has no room for `size`, without ever
 * calling memory__external_alloc() in that case: on a board where a "swap"
 * partition has been committed (see partition_manager__commit()),
 * memory__external_alloc() would otherwise land on it whenever PSRAM is
 * short, which means erasing a real 64 KiB flash region just to get a
 * SWAP-backed object this function immediately frees because it's not
 * safely writable through a raw pointer -- only through
 * memory__external_write(). That erase is real hardware time (a 64 KiB
 * region is 16 flash sectors) wasted on a result this never uses, and was
 * the whole cause of a plain terminal open going from instant to ~700ms
 * once a swap partition existed to find. memory__get_stats() only reads
 * already-tracked heap/page-bitmap counters, so checking it first is cheap
 * regardless of which way it comes out. */
static bruce_result_t
terminal__alloc_buffer(void **out_data, bruce_memory_object_t *out_object, bool *out_external, size_t size) {
    bruce_memory_stats_t stats;
    bool psram_has_room =
        memory__get_stats(&stats) == BRUCE_OK && stats.psram_largest_block >= size;
    bruce_memory_object_t object;
    if (psram_has_room && memory__external_alloc(size, &object) == BRUCE_OK) {
        const void *mapped = NULL;
        if ((object.backend == BRUCE_MEMORY_BACKEND_PSRAM ||
             object.backend == BRUCE_MEMORY_BACKEND_INTERNAL) &&
            memory__external_map(&object, &mapped) == BRUCE_OK) {
            memset((void *)mapped, 0, size);
            *out_data = (void *)mapped;
            *out_object = object;
            *out_external = true;
            return BRUCE_OK;
        }
        (void)memory__external_free(&object);
    }
    *out_data = memory__calloc(size, 1);
    *out_external = false;
    return *out_data != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
}

static void terminal__free_buffer(void *data, bruce_memory_object_t *object, bool external) {
    if (external) {
        (void)memory__external_free(object);
    } else {
        memory__free(data);
    }
}

static void terminal__free_buffers(terminal__state_t *state) {
    terminal__free_buffer(state->cells, &state->cells_object, state->cells_external);
    terminal__free_buffer(state->alt_cells, &state->alt_cells_object, state->alt_cells_external);
}

static void terminal__drain_output(terminal__state_t *state) {
    char output[256];
    size_t size = 0;
    while (stdio__session_read_output(state->session, output, sizeof(output), &size) == BRUCE_OK) {
        terminal_grid__feed(&state->grid, output, size);
        state->dirty = true;
        char response[TERMINAL_ANSI_RESPONSE_CAPACITY];
        size_t response_len = terminal_grid__take_response(&state->grid, response, sizeof(response));
        if (response_len > 0) (void)stdio__session_write_input(state->session, response, response_len);
    }
}

static void terminal__write_input(terminal__state_t *state, const char *bytes, size_t size) {
    (void)stdio__session_write_input(state->session, bytes, size);
}

/* Draws the active grid a row at a time, run-length-encoding consecutive
 * cells that share the same resolved (post-reverse-video) colors and
 * underline state into a single display__print() call. Erased/blank cells
 * are rendered as a real space so an opaque background (reverse video, a
 * colored status bar) still paints correctly. */
static void terminal__draw_grid(const terminal__state_t *state, uint16_t theme_fg, uint16_t theme_bg) {
    const terminal_grid_t *grid = &state->grid;
    const terminal_cell_t *cells = terminal_grid__active_cells(grid);
    char text[TERMINAL__MAX_COLUMNS * 4 + 1];
    for (uint16_t y = 0; y < grid->rows; ++y) {
        const terminal_cell_t *row = cells + (size_t)y * grid->columns;
        uint16_t x = 0;
        while (x < grid->columns) {
            const terminal_cell_t *first = &row[x];
            bool reverse = (first->attrs & TERMINAL_ANSI_ATTR_REVERSE) != 0;
            bool underline = (first->attrs & TERMINAL_ANSI_ATTR_UNDERLINE) != 0;
            uint16_t fg565 = terminal_ansi__color565(reverse ? first->bg : first->fg, reverse ? theme_bg : theme_fg);
            uint16_t bg565 = terminal_ansi__color565(reverse ? first->fg : first->bg, reverse ? theme_fg : theme_bg);
            uint16_t run = 0;
            size_t text_len = 0;
            while (x + run < grid->columns) {
                const terminal_cell_t *cell = &row[x + run];
                bool cell_reverse = (cell->attrs & TERMINAL_ANSI_ATTR_REVERSE) != 0;
                bool cell_underline = (cell->attrs & TERMINAL_ANSI_ATTR_UNDERLINE) != 0;
                if (cell_reverse != reverse || cell_underline != underline) break;
                int16_t cell_fg = cell_reverse ? cell->bg : cell->fg;
                int16_t cell_bg = cell_reverse ? cell->fg : cell->bg;
                int16_t first_fg = reverse ? first->bg : first->fg;
                int16_t first_bg = reverse ? first->fg : first->bg;
                if (cell_fg != first_fg || cell_bg != first_bg) break;
                if (cell->utf8_len == 0) {
                    text[text_len++] = ' ';
                } else {
                    memcpy(text + text_len, cell->utf8, cell->utf8_len);
                    text_len += cell->utf8_len;
                }
                run++;
            }
            text[text_len] = '\0';
            display__set_text_color(fg565);
            display__set_text_bg_color(bg565);
            display__set_cursor(
                TERMINAL__FRAME_MARGIN + (int16_t)(x * TERMINAL__CHAR_W), TERMINAL__TITLE_H + y * TERMINAL__CHAR_H
            );
            display__print(text);
            if (underline) {
                display__fill_rect(
                    TERMINAL__FRAME_MARGIN + (int16_t)(x * TERMINAL__CHAR_W),
                    TERMINAL__TITLE_H + y * TERMINAL__CHAR_H + TERMINAL__CHAR_H - 1, (int16_t)(run * TERMINAL__CHAR_W),
                    1, fg565
                );
            }
            x = (uint16_t)(x + run);
        }
    }
}

static void terminal__draw_cursor(const terminal__state_t *state, uint16_t theme_fg, uint16_t theme_bg) {
    const terminal_grid_t *grid = &state->grid;
    if (!grid->cursor_visible) return;
    int16_t x = TERMINAL__FRAME_MARGIN + (int16_t)(grid->cursor_x * TERMINAL__CHAR_W);
    int16_t y = TERMINAL__TITLE_H + grid->cursor_y * TERMINAL__CHAR_H;
    display__fill_rect(x, y, TERMINAL__CHAR_W, TERMINAL__CHAR_H, theme_fg);
    const terminal_cell_t *cells = terminal_grid__active_cells(grid);
    const terminal_cell_t *cell = &cells[(size_t)grid->cursor_y * grid->columns + grid->cursor_x];
    if (cell->utf8_len > 0) {
        char glyph[5];
        memcpy(glyph, cell->utf8, cell->utf8_len);
        glyph[cell->utf8_len] = '\0';
        display__set_text_color(theme_bg);
        display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
        display__set_cursor(x, y);
        display__print(glyph);
    }
}

static bruce_result_t terminal__draw(const terminal__state_t *state) {
    uint16_t foreground = config__get_theme_primary();
    uint16_t background = config__get_theme_background();
    int width = display__width();
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    display__fill_screen(background);
    display__fill_rect(0, 0, width, TERMINAL__TITLE_H, foreground);
    display__set_text_size(1);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_text_color(background);
    display__set_cursor(TERMINAL__FRAME_MARGIN, TERMINAL__FRAME_MARGIN);
    display__print(state->child != BRUCE_PROCESS_ID_INVALID ? "Terminal [running]" : "Terminal");
    terminal__draw_grid(state, foreground, background);
    terminal__draw_cursor(state, foreground, background);
    return display__present();
}

static void terminal__open_text_input(terminal__state_t *state) {
    char entered[TERMINAL__LINE_CAPACITY];
    if (dialog__text_input("Terminal", "input", NULL, false, entered, sizeof(entered)) == BRUCE_OK) {
        terminal__write_input(state, entered, strlen(entered));
        terminal__write_input(state, "\r", 1);
    }
    state->dirty = true;
}

static void terminal__handle_input(terminal__state_t *state, const bruce_input_event_t *event) {
    bool semantic_key = event->type == BRUCE_INPUT_KEY && event->value != event->code;
    if ((semantic_key && event->code == BRUCE_INPUT_CODE_BACK) ||
        (event->type == BRUCE_INPUT_KEY && event->code == TERMINAL__ASCII_ESCAPE) ||
        event->code == BRUCE_INPUT_CODE_BUTTON_B) {
        state->exit_requested = true;
        return;
    }
    if ((event->code == BRUCE_INPUT_CODE_SELECT && (semantic_key || event->type != BRUCE_INPUT_KEY)) ||
        event->code == BRUCE_INPUT_CODE_BUTTON_A) {
        terminal__open_text_input(state);
        return;
    }
    if (semantic_key || event->type != BRUCE_INPUT_KEY) {
        const char *sequence = NULL;
        switch (event->code) {
            case BRUCE_INPUT_CODE_UP: sequence = "\033[A"; break;
            case BRUCE_INPUT_CODE_DOWN: sequence = "\033[B"; break;
            case BRUCE_INPUT_CODE_RIGHT: sequence = "\033[C"; break;
            case BRUCE_INPUT_CODE_LEFT: sequence = "\033[D"; break;
            case BRUCE_INPUT_CODE_HOME: sequence = "\033[H"; break;
            case BRUCE_INPUT_CODE_DELETE: sequence = "\033[3~"; break;
            case BRUCE_INPUT_CODE_PREV: sequence = "\033[5~"; break; /* Page Up */
            case BRUCE_INPUT_CODE_NEXT: sequence = "\033[6~"; break; /* Page Down */
            default: break;
        }
        if (sequence != NULL) terminal__write_input(state, sequence, strlen(sequence));
        return;
    }
    if (event->code == '\n' || event->code == '\r') {
        terminal__write_input(state, "\r", 1);
    } else if (event->code == '\b' || event->code == TERMINAL__ASCII_DELETE) {
        terminal__write_input(state, "\177", 1);
    } else if (event->code > 0 && event->code <= TERMINAL__ASCII_DELETE) {
        char byte = (char)event->code;
        terminal__write_input(state, &byte, 1);
    }
}

/* Rebuilds a shell-style line from startup argv so the terminal submission
 * path can tokenize it again without changing argument boundaries. */
static bool terminal__args_to_line(ArgParser *parser, char *out, size_t out_size) {
    size_t used = 0;
    int argc = ap_count_args(parser);
    for (int i = 0; i < argc; ++i) {
        const char *arg = ap_get_arg_at_index(parser, i);
        size_t needed = i > 0 ? 1u : 0u;
        needed += 2;
        for (const char *p = arg; *p != '\0'; ++p) needed += (*p == '\\' || *p == '"') ? 2u : 1u;
        if (needed >= out_size - used) return false;
        if (i > 0) out[used++] = ' ';
        out[used++] = '"';
        for (const char *p = arg; *p != '\0'; ++p) {
            if (*p == '\\' || *p == '"') out[used++] = '\\';
            out[used++] = *p;
        }
        out[used++] = '"';
    }
    out[used] = '\0';
    return true;
}

int terminal_app_main(int argc, char **argv) {
    char startup_line[TERMINAL__LINE_CAPACITY] = {0};
    bool has_startup_command = false;
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Open the terminal and optionally run a startup command.");
    ap_add_flag(parser, "gui");
    ap_add_optional_arg(parser, "command", "Command to run on startup");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP) return 0;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    const char *command = ap_get_arg(parser, "command");
    has_startup_command = command != NULL;
    if (has_startup_command && !terminal__args_to_line(parser, startup_line, sizeof(startup_line))) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(parser);

    terminal__state_t state = {0};
    state.child = BRUCE_PROCESS_ID_INVALID;

    int width = display__width();
    int height = display__height();
    int columns = (width - 2 * TERMINAL__FRAME_MARGIN) / TERMINAL__CHAR_W;
    int rows = (height - TERMINAL__TITLE_H - 2 * TERMINAL__FRAME_MARGIN) / TERMINAL__CHAR_H;
    if (columns < 1) columns = 1;
    if (rows < 1) rows = 1;
    if (columns > TERMINAL__MAX_COLUMNS) columns = TERMINAL__MAX_COLUMNS;
    if (rows > TERMINAL__MAX_ROWS) rows = TERMINAL__MAX_ROWS;

    size_t cell_buffer_size = (size_t)columns * (size_t)rows * sizeof(terminal_cell_t);
    void *cells_data = NULL;
    void *alt_cells_data = NULL;
    bruce_result_t cells_alloc =
        terminal__alloc_buffer(&cells_data, &state.cells_object, &state.cells_external, cell_buffer_size);
    state.cells = cells_data;
    bruce_result_t alt_cells_alloc =
        terminal__alloc_buffer(&alt_cells_data, &state.alt_cells_object, &state.alt_cells_external, cell_buffer_size);
    state.alt_cells = alt_cells_data;
    if (cells_alloc != BRUCE_OK || alt_cells_alloc != BRUCE_OK) {
        if (cells_alloc == BRUCE_OK) terminal__free_buffer(state.cells, &state.cells_object, state.cells_external);
        if (alt_cells_alloc == BRUCE_OK) {
            terminal__free_buffer(state.alt_cells, &state.alt_cells_object, state.alt_cells_external);
        }
        return BRUCE_ERR_NO_MEMORY;
    }
    terminal_grid__init(&state.grid, state.cells, state.alt_cells, (uint16_t)columns, (uint16_t)rows);
    terminal_grid__feed(
        &state.grid, "Bruce terminal\r\nType a command and press Enter.\r\n",
        strlen("Bruce terminal\r\nType a command and press Enter.\r\n")
    );
    (void)input__flush();

    if (stdio__session_create(&state.session) != BRUCE_OK) {
        terminal__free_buffers(&state);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    /* Establishes the session's tty geometry before the shell (and every
     * descendant it launches) starts, so $COLUMNS/$LINES and stty/tty__*
     * queries all see the real screen size from their very first read --
     * see core_sdk/tty.h and shell_app.c's shell__sync_tty_size. */
    (void)tty__set_size(state.session, (uint16_t)columns, (uint16_t)rows);

    (void)stdio__session_route_children(state.session);
    int shell_process = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (shell_process <= 0) {
        (void)stdio__session_close(state.session);
        terminal__free_buffers(&state);
        return shell_process;
    }
    state.child = (bruce_process_id_t)shell_process;
    state.dirty = true;

    if (has_startup_command) {
        terminal__write_input(&state, startup_line, strlen(startup_line));
        terminal__write_input(&state, "\r", 1);
    }

    while (!state.exit_requested) {
        terminal__drain_output(&state);
        if (state.child != BRUCE_PROCESS_ID_INVALID) {
            bruce_process_snapshot_t child;
            if (process__snapshot(state.child, &child) != BRUCE_OK) {
                bruce_process_status_t status;
                (void)process__wait_status(state.child, 0, &status);
                state.child = BRUCE_PROCESS_ID_INVALID;
                state.exit_requested = true;
                state.dirty = true;
            }
        }
        if (state.dirty) {
            bruce_result_t draw = terminal__draw(&state);
            if (draw == BRUCE_OK) state.dirty = false;
        }

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, TERMINAL__INPUT_TIMEOUT_MS);
        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            (void)runtime__delay(TERMINAL__HIDDEN_DELAY_MS);
            state.dirty = true;
            continue;
        }
        if (input_result != BRUCE_OK || event.action != BRUCE_INPUT_PRESS) continue;
        terminal__handle_input(&state, &event);
    }

    if (state.child != BRUCE_PROCESS_ID_INVALID) {
        (void)process__terminate(state.child);
        bruce_process_status_t status;
        if (process__wait_status(state.child, TERMINAL__CHILD_STOP_TIMEOUT_MS, &status) != BRUCE_OK) {
            (void)process__kill(state.child);
            (void)process__wait_status(state.child, TERMINAL__CHILD_STOP_TIMEOUT_MS, &status);
        }
    }
    (void)stdio__session_close(state.session);
    terminal__free_buffers(&state);
    return 0;
}
