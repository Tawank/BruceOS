#include "terminal_app.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/clipboard.h"
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
#define TERMINAL__ASCII_INTERRUPT 0x03
#define TERMINAL__ASCII_PASTE 0x16

/* User-adjustable font size, a direct display__set_text_size() multiplier
 * (unlike browser's per-heading-level delta -- the terminal only ever draws
 * one text size). Adjusted via BRUCE_INPUT_CODE_ZOOM_OUT/IN (Fn + -/=, see
 * input_keyboard.c) rather than plain +/- keystrokes: every printable key
 * here is live shell input (see terminal__handle_input), so there's no key
 * left to steal for it. 0.5 bottoms out readable at this font's 6x10 native
 * cell; 3.0 is plenty to make the grid unusably coarse before it stops being
 * useful, same ceiling as browser's zoom. */
#define TERMINAL__FONT_SCALE_MIN 0.5f
#define TERMINAL__FONT_SCALE_MAX 3.0f
#define TERMINAL__FONT_SCALE_STEP 0.5f
#define TERMINAL__FONT_SCALE_DEFAULT 1.0f

typedef struct {
    terminal_cell_t *cells;
    terminal_cell_t *alt_cells;
    bool cells_external;
    bool alt_cells_external;
    terminal_grid_t grid;
    bruce_stdio_session_t session;
    bruce_process_id_t child;
    bool dirty;
    bool exit_requested;
    float font_scale;      /* See TERMINAL__FONT_SCALE_MIN/MAX above. */
    size_t cell_capacity;  /* Cells `cells`/`alt_cells` can each hold -- see terminal__apply_font_scale(). */

    /* Keyboard-driven copy mode -- there's no mouse to drag-select with, so
     * this is this terminal's take on tmux/screen's copy-mode: SELECT/BTN_A
     * opens a menu (terminal__open_actions_menu) offering "Select text",
     * which moves an independent selection cursor over the grid instead of
     * forwarding keys to the child (see terminal__handle_selection_input).
     * Meaningless while `selecting` is false. */
    bool selecting;
    bool selection_active; /* Anchor has been dropped; still false right after entering selection mode. */
    uint16_t sel_cursor_x;
    uint16_t sel_cursor_y;
    uint16_t sel_anchor_x;
    uint16_t sel_anchor_y;
} terminal__state_t;

/* Grid geometry (columns/rows) that fits the display at `scale`, using the
 * same math -- and the same TERMINAL__MAX_COLUMNS/ROWS clamp -- as the
 * startup sizing this factors out of. */
static void terminal__grid_size(float scale, int *out_columns, int *out_rows) {
    int width = display__width();
    int height = display__height();
    int char_w = (int)lroundf((float)TERMINAL__CHAR_W * scale);
    int char_h = (int)lroundf((float)TERMINAL__CHAR_H * scale);
    if (char_w < 1) char_w = 1;
    if (char_h < 1) char_h = 1;
    int columns = (width - 2 * TERMINAL__FRAME_MARGIN) / char_w;
    int rows = (height - TERMINAL__TITLE_H - 2 * TERMINAL__FRAME_MARGIN) / char_h;
    if (columns < 1) columns = 1;
    if (rows < 1) rows = 1;
    if (columns > TERMINAL__MAX_COLUMNS) columns = TERMINAL__MAX_COLUMNS;
    if (rows > TERMINAL__MAX_ROWS) rows = TERMINAL__MAX_ROWS;
    *out_columns = columns;
    *out_rows = rows;
}

/* Prefers a PSRAM- or plain-internal-RAM-backed memory__external allocation
 * (both are plain, directly addressable buffers, guaranteed by
 * memory__external_malloc_writable() which never lands on flash-backed swap)
 * so the grid can keep mutating it in place with ordinary pointer writes.
 * Falls back to a separate, untracked internal heap allocation when neither
 * has room for `size`. */
static bruce_result_t terminal__alloc_buffer(void **out_data, bool *out_external, size_t size) {
    const void *data = memory__external_malloc_writable(size);
    if (data != NULL) {
        memset((void *)data, 0, size);
        *out_data = (void *)data;
        *out_external = true;
        return BRUCE_OK;
    }
    *out_data = memory__calloc(size, 1);
    *out_external = false;
    return *out_data != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
}

static void terminal__free_buffer(void *data, bool external) {
    if (external) {
        (void)memory__external_free(data);
    } else {
        memory__free(data);
    }
}

static void terminal__free_buffers(terminal__state_t *state) {
    terminal__free_buffer(state->cells, state->cells_external);
    terminal__free_buffer(state->alt_cells, state->alt_cells_external);
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
    /* Matches display__print()'s own per-glyph advance -- lroundf(cell_width
     * * text_size) -- so a run's start x lines up with wherever the previous
     * run's glyphs actually landed instead of drifting from it. */
    int char_w = (int)lroundf((float)TERMINAL__CHAR_W * state->font_scale);
    int char_h = (int)lroundf((float)TERMINAL__CHAR_H * state->font_scale);
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
            display__set_cursor(TERMINAL__FRAME_MARGIN + (int16_t)(x * char_w), TERMINAL__TITLE_H + y * char_h);
            display__print(text);
            if (underline) {
                display__fill_rect(
                    TERMINAL__FRAME_MARGIN + (int16_t)(x * char_w), TERMINAL__TITLE_H + y * char_h + char_h - 1,
                    (int16_t)(run * char_w), 1, fg565
                );
            }
            x = (uint16_t)(x + run);
        }
    }
}

/* Draws a solid cursor block at grid cell (column, row), with the glyph
 * already there (if any) redrawn in `theme_text` on top so it stays legible.
 * Shared by the real terminal cursor and the selection-mode cursor below --
 * they're never both on screen at once (see terminal__draw()). */
static void terminal__draw_cursor_at(
    const terminal__state_t *state, uint16_t column, uint16_t row, uint16_t theme_fg, uint16_t theme_text
) {
    const terminal_grid_t *grid = &state->grid;
    int char_w = (int)lroundf((float)TERMINAL__CHAR_W * state->font_scale);
    int char_h = (int)lroundf((float)TERMINAL__CHAR_H * state->font_scale);
    int16_t x = TERMINAL__FRAME_MARGIN + (int16_t)(column * char_w);
    int16_t y = TERMINAL__TITLE_H + row * char_h;
    display__fill_rect(x, y, (int16_t)char_w, (int16_t)char_h, theme_fg);
    const terminal_cell_t *cells = terminal_grid__active_cells(grid);
    const terminal_cell_t *cell = &cells[(size_t)row * grid->columns + column];
    if (cell->utf8_len > 0) {
        char glyph[5];
        memcpy(glyph, cell->utf8, cell->utf8_len);
        glyph[cell->utf8_len] = '\0';
        display__set_text_color(theme_text);
        display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
        display__set_cursor(x, y);
        display__print(glyph);
    }
}

/* `theme_fg`/`theme_bg` are the ANSI default foreground/background (primary/
 * background - see terminal__draw_grid()); `theme_text` is the chrome text
 * color the inverted glyph under the cursor block is drawn in, so it stays
 * legible against theme_fg regardless of what theme_bg happens to be. */
static void
terminal__draw_cursor(const terminal__state_t *state, uint16_t theme_fg, uint16_t theme_bg, uint16_t theme_text) {
    (void)theme_bg;
    if (!state->grid.cursor_visible) return;
    terminal__draw_cursor_at(state, state->grid.cursor_x, state->grid.cursor_y, theme_fg, theme_text);
}

/* Normalizes the anchor/cursor pair into a row-major (start <= end) range,
 * the same order terminal__copy_selection() walks the grid in. */
static void terminal__selection_range(
    const terminal__state_t *state, uint16_t *out_start_y, uint16_t *out_start_x, uint16_t *out_end_y,
    uint16_t *out_end_x
) {
    uint16_t ax = state->sel_anchor_x, ay = state->sel_anchor_y;
    uint16_t cx = state->sel_cursor_x, cy = state->sel_cursor_y;
    bool anchor_first = ay < cy || (ay == cy && ax <= cx);
    *out_start_y = anchor_first ? ay : cy;
    *out_start_x = anchor_first ? ax : cx;
    *out_end_y = anchor_first ? cy : ay;
    *out_end_x = anchor_first ? cx : ax;
}

/* Paints the highlighted range (once an anchor has been dropped) as solid
 * `theme_fg`-on-`theme_text` blocks, like a real terminal's selection
 * highlight -- character-wise (an xterm mouse drag), not the rectangular
 * "block select" some terminals offer as an alt-modifier variant. */
static void terminal__draw_selection(const terminal__state_t *state, uint16_t theme_fg, uint16_t theme_text) {
    if (!state->selection_active) return;
    const terminal_grid_t *grid = &state->grid;
    const terminal_cell_t *cells = terminal_grid__active_cells(grid);
    int char_w = (int)lroundf((float)TERMINAL__CHAR_W * state->font_scale);
    int char_h = (int)lroundf((float)TERMINAL__CHAR_H * state->font_scale);
    uint16_t start_y, start_x, end_y, end_x;
    terminal__selection_range(state, &start_y, &start_x, &end_y, &end_x);
    char text[TERMINAL__MAX_COLUMNS * 4 + 1];
    for (uint16_t y = start_y; y <= end_y; ++y) {
        uint16_t col_start = (y == start_y) ? start_x : 0;
        uint16_t col_end = (y == end_y) ? end_x : (uint16_t)(grid->columns - 1);
        const terminal_cell_t *row = cells + (size_t)y * grid->columns;
        size_t text_len = 0;
        for (uint16_t x = col_start; x <= col_end; ++x) {
            const terminal_cell_t *cell = &row[x];
            if (cell->utf8_len == 0) {
                text[text_len++] = ' ';
            } else {
                memcpy(text + text_len, cell->utf8, cell->utf8_len);
                text_len += cell->utf8_len;
            }
        }
        text[text_len] = '\0';
        display__set_text_color(theme_text);
        display__set_text_bg_color(theme_fg);
        display__set_cursor(TERMINAL__FRAME_MARGIN + (int16_t)(col_start * char_w), TERMINAL__TITLE_H + y * char_h);
        display__print(text);
    }
}

static bruce_result_t terminal__draw(const terminal__state_t *state) {
    uint16_t foreground = config__get_color_primary();
    uint16_t background = config__get_color_background();
    uint16_t text_color = config__get_color_text();
    int width = display__width();
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    display__fill_screen(background);
    display__fill_rect(0, 0, width, TERMINAL__TITLE_H, foreground);
    display__set_text_size(1);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__set_text_color(text_color);
    display__set_cursor(TERMINAL__FRAME_MARGIN, TERMINAL__FRAME_MARGIN);
    display__print(
        state->selecting            ? "Terminal [v=mark y=copy Esc=cancel]"
        : state->child != BRUCE_PROCESS_ID_INVALID ? "Terminal [running]"
                                                    : "Terminal"
    );
    /* The title bar above is always drawn at size 1 regardless of
     * `font_scale` -- it's fixed chrome, not grid content, and staying
     * legible at TERMINAL__TITLE_H matters more than matching the zoom. */
    display__set_text_size(state->font_scale);
    terminal__draw_grid(state, foreground, background);
    if (state->selecting) {
        terminal__draw_selection(state, foreground, text_color);
        terminal__draw_cursor_at(state, state->sel_cursor_x, state->sel_cursor_y, foreground, text_color);
    } else {
        terminal__draw_cursor(state, foreground, background, text_color);
    }
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

/* Re-fits the grid to `new_scale` (clamped to TERMINAL__FONT_SCALE_MIN/MAX)
 * and pushes the new geometry to the child through the same tty__set_size()
 * live-resize path ssh_app.c uses for a remote pty -- a well-behaved program
 * (the shell, vim, htop, ...) picks up the new columns/rows next time it
 * polls tty__get_size()'s generation, same as reacting to a real SIGWINCH.
 * terminal_grid__init() clears the screen and scrollback, matching what a
 * real terminal emulator's resize does. The `==` guard skips that reset (and
 * the resulting flicker) when the scale didn't actually change, e.g. already
 * at MIN/MAX and pressed again -- safe because every reachable value is an
 * exact sum of TERMINAL__FONT_SCALE_STEP (0.5, a power of two) starting from
 * TERMINAL__FONT_SCALE_DEFAULT, so there's no float drift to worry about.
 *
 * The cell buffers only ever grow, lazily, the first time a scale actually
 * needs more cells than they currently hold -- they're never sized up front
 * for TERMINAL__FONT_SCALE_MIN's grid (the largest one reachable). That
 * would always cost ~4x this app's steady-state heap use (smaller font =
 * more, smaller cells) whether or not a session ever zooms, and on a board
 * with no PSRAM (e.g. Cardputer, the only keyboard-equipped board this
 * feature reaches) that 4x comes straight out of scarce internal SRAM. */
static void terminal__apply_font_scale(terminal__state_t *state, float new_scale) {
    if (new_scale < TERMINAL__FONT_SCALE_MIN) new_scale = TERMINAL__FONT_SCALE_MIN;
    if (new_scale > TERMINAL__FONT_SCALE_MAX) new_scale = TERMINAL__FONT_SCALE_MAX;
    if (new_scale == state->font_scale) return;
    int columns, rows;
    terminal__grid_size(new_scale, &columns, &rows);
    size_t needed_cells = (size_t)columns * (size_t)rows;
    if (needed_cells > state->cell_capacity) {
        size_t new_buffer_size = needed_cells * sizeof(terminal_cell_t);
        void *new_cells = NULL;
        void *new_alt_cells = NULL;
        bool new_cells_external = false;
        bool new_alt_cells_external = false;
        bruce_result_t cells_alloc = terminal__alloc_buffer(&new_cells, &new_cells_external, new_buffer_size);
        bruce_result_t alt_cells_alloc =
            terminal__alloc_buffer(&new_alt_cells, &new_alt_cells_external, new_buffer_size);
        if (cells_alloc != BRUCE_OK || alt_cells_alloc != BRUCE_OK) {
            if (cells_alloc == BRUCE_OK) terminal__free_buffer(new_cells, new_cells_external);
            if (alt_cells_alloc == BRUCE_OK) {
                terminal__free_buffer(new_alt_cells, new_alt_cells_external);
            }
            return; /* Out of memory: stay at the current, still-valid scale rather than corrupt it. */
        }
        terminal__free_buffer(state->cells, state->cells_external);
        terminal__free_buffer(state->alt_cells, state->alt_cells_external);
        state->cells = new_cells;
        state->alt_cells = new_alt_cells;
        state->cells_external = new_cells_external;
        state->alt_cells_external = new_alt_cells_external;
        state->cell_capacity = needed_cells;
    }
    terminal_grid__init(&state->grid, state->cells, state->alt_cells, (uint16_t)columns, (uint16_t)rows);
    (void)tty__set_size(state->session, (uint16_t)columns, (uint16_t)rows);
    state->font_scale = new_scale;
    state->dirty = true;
}

/* Writes the clipboard's text to the child as if typed -- same as Ctrl+V
 * below and text_app.c's Ctrl+V. Silently does nothing if the clipboard
 * doesn't hold text (e.g. it's empty, or holds files from the file
 * manager). */
static void terminal__paste_clipboard(terminal__state_t *state) {
    if (clipboard__kind() != BRUCE_CLIPBOARD_TEXT) return;
    const char *text = clipboard__get_text();
    if (text != NULL && *text != '\0') terminal__write_input(state, text, strlen(text));
}

/* Extracts the highlighted range (row-major, character-wise -- like an
 * xterm mouse drag, not a rectangular/block select) into a freshly
 * allocated string and puts it on the shared clipboard. Trailing blank
 * (never-written) cells on each copied line are trimmed, the way a real
 * terminal doesn't copy the unwritten padding after a short line. */
static void terminal__copy_selection(const terminal__state_t *state) {
    const terminal_grid_t *grid = &state->grid;
    const terminal_cell_t *cells = terminal_grid__active_cells(grid);
    uint16_t start_y, start_x, end_y, end_x;
    terminal__selection_range(state, &start_y, &start_x, &end_y, &end_x);

    size_t rows = (size_t)(end_y - start_y) + 1;
    size_t capacity = rows * ((size_t)grid->columns * 4u + 1u) + 1u;
    char *text = memory__malloc(capacity);
    if (text == NULL) return;
    size_t len = 0;
    for (uint16_t y = start_y; y <= end_y; ++y) {
        uint16_t col_start = (y == start_y) ? start_x : 0;
        uint16_t col_end = (y == end_y) ? end_x : (uint16_t)(grid->columns - 1);
        const terminal_cell_t *row = cells + (size_t)y * grid->columns;
        size_t line_start = len;
        for (uint16_t x = col_start; x <= col_end; ++x) {
            const terminal_cell_t *cell = &row[x];
            if (cell->utf8_len == 0) {
                text[len++] = ' ';
            } else {
                memcpy(text + len, cell->utf8, cell->utf8_len);
                len += cell->utf8_len;
            }
        }
        while (len > line_start && text[len - 1] == ' ') len--; /* trim trailing padding */
        if (y != end_y) text[len++] = '\n';
    }
    text[len] = '\0';
    (void)clipboard__set_text(text);
    memory__free(text);
}

static void terminal__enter_selection(terminal__state_t *state) {
    state->selecting = true;
    state->selection_active = false;
    state->sel_cursor_x = state->grid.cursor_x;
    state->sel_cursor_y = state->grid.cursor_y;
    state->sel_anchor_x = state->sel_cursor_x;
    state->sel_anchor_y = state->sel_cursor_y;
    state->dirty = true;
}

static void terminal__exit_selection(terminal__state_t *state) {
    state->selecting = false;
    state->selection_active = false;
    state->dirty = true;
}

/* Keyboard-driven copy mode, modeled on tmux/WezTerm/Alacritty's vi-style
 * copy-mode bindings (the scheme that keeps recurring across terminals that
 * support keyboard-only selection): h/j/k/l or arrows move the selection
 * cursor, `v` drops the anchor, `y` (or Enter) confirms and copies,
 * Escape/Back cancels. Every other key is silently absorbed -- nothing is
 * forwarded to the child while selecting. */
static void terminal__handle_selection_input(terminal__state_t *state, const bruce_input_event_t *event) {
    bool semantic_key = event->type == BRUCE_INPUT_KEY && event->value != event->code;
    const terminal_grid_t *grid = &state->grid;

    if ((semantic_key && event->code == BRUCE_INPUT_CODE_BACK) ||
        (event->type == BRUCE_INPUT_KEY && event->code == TERMINAL__ASCII_ESCAPE) ||
        event->code == BRUCE_INPUT_CODE_BUTTON_B) {
        terminal__exit_selection(state);
        return;
    }

    /* `v` toggles the anchor, matching vim: press once to mark, press again
     * (still nothing else typed) to cancel the mark and keep browsing. */
    bool mark = event->type == BRUCE_INPUT_KEY && !semantic_key && event->code == 'v';
    /* `y`/Enter/BTN_A/SELECT confirm: with no anchor yet, they mark here (so
     * a single confirm press still works on boards with no `v` key);
     * with an anchor already set, they copy the highlighted range and leave
     * selection mode -- the same two-step flow tmux's `y` and tmux's plain
     * Enter both follow. */
    bool confirm = event->code == BRUCE_INPUT_CODE_BUTTON_A ||
                   (event->code == BRUCE_INPUT_CODE_SELECT && event->type != BRUCE_INPUT_KEY) ||
                   (event->type == BRUCE_INPUT_KEY && !semantic_key &&
                    (event->code == '\r' || event->code == '\n' || event->code == 'y'));
    if (mark) {
        state->selection_active = !state->selection_active;
        if (state->selection_active) {
            state->sel_anchor_x = state->sel_cursor_x;
            state->sel_anchor_y = state->sel_cursor_y;
        }
        state->dirty = true;
        return;
    }
    if (confirm) {
        if (!state->selection_active) {
            state->selection_active = true;
            state->sel_anchor_x = state->sel_cursor_x;
            state->sel_anchor_y = state->sel_cursor_y;
            state->dirty = true;
            return;
        }
        terminal__copy_selection(state);
        terminal__exit_selection(state);
        return;
    }

    int32_t code = event->code;
    if (event->type == BRUCE_INPUT_KEY && !semantic_key) {
        /* hjkl aliases, vi-style -- only when not shadowing a semantic code. */
        if (code == 'h') code = BRUCE_INPUT_CODE_LEFT;
        else if (code == 'j') code = BRUCE_INPUT_CODE_DOWN;
        else if (code == 'k') code = BRUCE_INPUT_CODE_UP;
        else if (code == 'l') code = BRUCE_INPUT_CODE_RIGHT;
    }
    if (semantic_key || event->type != BRUCE_INPUT_KEY || code != event->code) {
        switch (code) {
            case BRUCE_INPUT_CODE_UP:
                if (state->sel_cursor_y > 0) state->sel_cursor_y--;
                break;
            case BRUCE_INPUT_CODE_DOWN:
                if ((uint16_t)(state->sel_cursor_y + 1) < grid->rows) state->sel_cursor_y++;
                break;
            case BRUCE_INPUT_CODE_LEFT:
                if (state->sel_cursor_x > 0) state->sel_cursor_x--;
                break;
            case BRUCE_INPUT_CODE_RIGHT:
                if ((uint16_t)(state->sel_cursor_x + 1) < grid->columns) state->sel_cursor_x++;
                break;
            default: return; /* unrecognized key: ignore */
        }
        state->dirty = true;
    }
}

/* SELECT/BTN_A opens a small actions menu -- the same entry point that used
 * to jump straight to dialog__text_input (still offered as "Type
 * command...") now also offers "Select text" (this terminal's take on
 * copy-mode, since there's no mouse to drag-select with -- see
 * terminal__handle_selection_input) and "Paste" (mirrors Ctrl+V below, for
 * boards that can't type a Ctrl chord at all). */
static void terminal__open_actions_menu(terminal__state_t *state) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Type command...", .value = "type"  },
        {.label = "Enter",           .value = "enter" },
        {.label = "Select text...",  .value = "select"},
        {.label = "Paste",           .value = "paste" },
        {.label = "Exit terminal",   .value = "exit"  },
        {.label = "Cancel",          .value = "cancel"},
    };
    size_t selected = 0;
    bruce_result_t result =
        dialog__choice("Terminal", NULL, choices, sizeof(choices) / sizeof(choices[0]), &selected);
    /* The dialog owns the most recently presented frame. Always repaint the
     * terminal after it closes, including when Cancel/Back dismissed it. */
    state->dirty = true;
    if (result != BRUCE_OK) return;
    if (strcmp(choices[selected].value, "type") == 0) {
        terminal__open_text_input(state);
    } else if (strcmp(choices[selected].value, "enter") == 0) {
        terminal__write_input(state, "\r", 1);
    } else if (strcmp(choices[selected].value, "select") == 0) {
        terminal__enter_selection(state);
    } else if (strcmp(choices[selected].value, "paste") == 0) {
        terminal__paste_clipboard(state);
    } else if (strcmp(choices[selected].value, "exit") == 0) {
        state->exit_requested = true;
    }
}

static void terminal__handle_input(terminal__state_t *state, const bruce_input_event_t *event) {
    if (state->selecting) {
        terminal__handle_selection_input(state, event);
        return;
    }
    bool semantic_key = event->type == BRUCE_INPUT_KEY && event->value != event->code;
    if ((semantic_key && event->code == BRUCE_INPUT_CODE_BACK) ||
        (event->type == BRUCE_INPUT_KEY && event->code == TERMINAL__ASCII_ESCAPE) ||
        event->code == BRUCE_INPUT_CODE_BUTTON_B) {
        state->exit_requested = true;
        return;
    }
    if ((event->code == BRUCE_INPUT_CODE_SELECT && (semantic_key || event->type != BRUCE_INPUT_KEY)) ||
        event->code == BRUCE_INPUT_CODE_BUTTON_A) {
        terminal__open_actions_menu(state);
        return;
    }
    if (semantic_key && event->code == BRUCE_INPUT_CODE_ZOOM_OUT) {
        terminal__apply_font_scale(state, state->font_scale - TERMINAL__FONT_SCALE_STEP);
        return;
    }
    if (semantic_key && event->code == BRUCE_INPUT_CODE_ZOOM_IN) {
        terminal__apply_font_scale(state, state->font_scale + TERMINAL__FONT_SCALE_STEP);
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
            case BRUCE_INPUT_CODE_PREV: sequence = "\033[A"; break;
            case BRUCE_INPUT_CODE_NEXT: sequence = "\033[B"; break;
            default: break;
        }
        if (sequence != NULL) terminal__write_input(state, sequence, strlen(sequence));
        return;
    }
    if (event->code == TERMINAL__ASCII_INTERRUPT && state->child != BRUCE_PROCESS_ID_INVALID) {
        /* Like a real terminal: Ctrl+C doesn't go to the child as a byte, it
         * sends SIGINT. `stty raw` (see bnu_sys_app.c) opts a program out of
         * this, so it gets the literal ^C byte instead -- e.g. a pager. */
        bruce_tty_mode_t mode = BRUCE_TTY_MODE_COOKED;
        (void)tty__get_mode_of(state->session, &mode);
        if (mode != BRUCE_TTY_MODE_RAW) {
            terminal_grid__feed(&state->grid, "^C\r\n", 4);
            state->dirty = true;
            /* Also discard whatever's already queued but not yet read (a
             * fast paste/burst typed just before Ctrl+C) -- see
             * stdio__session_flush_input()'s doc: a real tty flushes pending
             * input on SIGINT the same way, and without this the leftover
             * bytes would survive into the child's next read and get
             * replayed as if freshly typed. */
            (void)stdio__session_flush_input(state->session);
            (void)process__signal(state->child, BRUCE_PROCESS_SIGNAL_INT);
            return;
        }
    }
    if (event->code == TERMINAL__ASCII_PASTE) {
        /* Ctrl+V pastes the clipboard, mirroring text_app.c's Ctrl+V --
         * gated on cooked mode the same way Ctrl+C is above, so a raw-mode
         * program that wants a literal Ctrl+V (vim's block-visual mode,
         * readline's quoted-insert) still gets the real byte instead of
         * having it hijacked. */
        bruce_tty_mode_t mode = BRUCE_TTY_MODE_COOKED;
        (void)tty__get_mode_of(state->session, &mode);
        if (mode != BRUCE_TTY_MODE_RAW) {
            terminal__paste_clipboard(state);
            return;
        }
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
    /* Heap-allocated, not a local char[TERMINAL__LINE_CAPACITY]: its content
     * is only needed for the one terminal__write_input() below, right before
     * the interactive loop starts, but a plain stack array declared at
     * function scope stays reserved for the rest of terminal_app_main's
     * activation - including every later terminal__handle_input() ->
     * terminal__open_actions_menu() -> dialog__choice() call the SELECT
     * actions menu makes, which is exactly the deep GUI-rendering chain this
     * task's stack is tightest for. Freed right after its one use below. */
    char *startup_line = NULL;
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
    if (has_startup_command) {
        startup_line = memory__malloc(TERMINAL__LINE_CAPACITY);
        if (startup_line == NULL || !terminal__args_to_line(parser, startup_line, TERMINAL__LINE_CAPACITY)) {
            bruce_result_t error = startup_line == NULL ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
            memory__free(startup_line);
            ap_free(parser);
            return error;
        }
    }
    ap_free(parser);

    terminal__state_t state = {0};
    state.child = BRUCE_PROCESS_ID_INVALID;

    state.font_scale = TERMINAL__FONT_SCALE_DEFAULT;

    int columns, rows;
    terminal__grid_size(state.font_scale, &columns, &rows);

    size_t cell_buffer_size = (size_t)columns * (size_t)rows * sizeof(terminal_cell_t);
    void *cells_data = NULL;
    void *alt_cells_data = NULL;
    bruce_result_t cells_alloc = terminal__alloc_buffer(&cells_data, &state.cells_external, cell_buffer_size);
    state.cells = cells_data;
    bruce_result_t alt_cells_alloc =
        terminal__alloc_buffer(&alt_cells_data, &state.alt_cells_external, cell_buffer_size);
    state.alt_cells = alt_cells_data;
    if (cells_alloc != BRUCE_OK || alt_cells_alloc != BRUCE_OK) {
        if (cells_alloc == BRUCE_OK) terminal__free_buffer(state.cells, state.cells_external);
        if (alt_cells_alloc == BRUCE_OK) {
            terminal__free_buffer(state.alt_cells, state.alt_cells_external);
        }
        memory__free(startup_line);
        return BRUCE_ERR_NO_MEMORY;
    }
    state.cell_capacity = (size_t)columns * (size_t)rows;
    terminal_grid__init(&state.grid, state.cells, state.alt_cells, (uint16_t)columns, (uint16_t)rows);
    terminal_grid__feed(
        &state.grid, "Bruce terminal\r\nType a command and press Enter.\r\n",
        strlen("Bruce terminal\r\nType a command and press Enter.\r\n")
    );
    (void)input__flush();

    if (stdio__session_create(&state.session) != BRUCE_OK) {
        terminal__free_buffers(&state);
        memory__free(startup_line);
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
        memory__free(startup_line);
        return shell_process;
    }
    state.child = (bruce_process_id_t)shell_process;
    state.dirty = true;

    if (has_startup_command) {
        terminal__write_input(&state, startup_line, strlen(startup_line));
        terminal__write_input(&state, "\r", 1);
    }
    memory__free(startup_line);
    startup_line = NULL;

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
