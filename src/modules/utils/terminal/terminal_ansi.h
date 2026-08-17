#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Cell-grid VT100/xterm-subset terminal emulator.
 *
 * This replaces the old flat-buffer "reflow the whole transcript every
 * frame" model with the model real terminal emulators use: a fixed
 * columns x rows grid of cells. Every printable byte the child process
 * writes lands in grid[cursor_y][cursor_x] with the current SGR attributes
 * baked in; escape/control sequences move the cursor, scroll a region,
 * switch to the alternate screen, etc. The renderer (terminal_app.c) never
 * interprets escape sequences itself -- it just walks the active grid and
 * paints cells, the same way a real xterm's redraw path only looks at its
 * own screen buffer.
 *
 * Deliberately not implemented (unsupported, ignored gracefully rather than
 * breaking the parser): truecolor SGR (38/48;2;r;g;b), italic/blink/strike
 * SGR, tab stops other than fixed 8-column, IRM/LNM, and any scrollback
 * beyond the current screen -- none of these are needed for htop/less/vim/
 * tmux to render correctly, which is the bar this was built to clear.
 */

#define TERMINAL_ANSI_COLOR_DEFAULT ((int16_t)-1)
#define TERMINAL_ANSI_CSI_MAX_PARAMS 16
#define TERMINAL_ANSI_RESPONSE_CAPACITY 32

typedef enum {
    TERMINAL_ANSI_ATTR_BOLD = 1u << 0,
    TERMINAL_ANSI_ATTR_DIM = 1u << 1,
    TERMINAL_ANSI_ATTR_UNDERLINE = 1u << 2,
    TERMINAL_ANSI_ATTR_REVERSE = 1u << 3,
} terminal_ansi_attr_t;

/* One screen cell. `utf8_len == 0` means blank (render as a space). Colors
 * are TERMINAL_ANSI_COLOR_DEFAULT or an xterm 256-color palette index. */
typedef struct {
    uint8_t utf8[4];
    uint8_t utf8_len;
    int16_t fg;
    int16_t bg;
    uint8_t attrs;
} terminal_cell_t;

typedef enum {
    TERMINAL_ANSI_TEXT,
    TERMINAL_ANSI_ESCAPE,
    TERMINAL_ANSI_CSI,
    TERMINAL_ANSI_OSC,
} terminal_ansi_parse_state_t;

typedef struct {
    /* Screen geometry. `cells`/`alt_cells` are caller-owned buffers of
     * columns * rows terminal_cell_t each -- see terminal_grid__init(). */
    terminal_cell_t *cells;
    terminal_cell_t *alt_cells;
    uint16_t columns;
    uint16_t rows;
    bool alt_screen_active;

    /* Cursor. */
    uint16_t cursor_x;
    uint16_t cursor_y;
    bool cursor_visible;
    /* Deferred line-wrap: set once a glyph is printed in the last column,
     * cleared (and the wrap actually performed) by whatever comes next.
     * Matches real terminals so a full-width line doesn't leave a phantom
     * blank row underneath it, and so the cursor still visually sits on
     * the last column until something forces it to move. */
    bool pending_wrap;

    /* Saved cursor, for DECSC/DECRC (ESC 7 / ESC 8) and the alt-screen
     * enter/leave sequences that implicitly save/restore it. */
    uint16_t saved_cursor_x;
    uint16_t saved_cursor_y;
    int16_t saved_fg;
    int16_t saved_bg;
    uint8_t saved_attrs;
    bool saved_origin_mode;

    /* Scroll region, 0-based inclusive rows. */
    uint16_t scroll_top;
    uint16_t scroll_bottom;

    /* Modes. */
    bool autowrap;
    bool origin_mode;

    /* Current SGR state applied to newly-written cells. */
    int16_t fg;
    int16_t bg;
    uint8_t attrs;

    /* Byte-stream parser state. */
    terminal_ansi_parse_state_t state;
    uint16_t value;
    bool has_value;
    uint16_t params[TERMINAL_ANSI_CSI_MAX_PARAMS];
    uint8_t param_count;
    bool csi_private;

    /* In-progress multi-byte UTF-8 sequence being assembled from raw
     * child-process bytes before it becomes a single cell glyph. */
    uint8_t utf8_pending[4];
    uint8_t utf8_pending_len;
    uint8_t utf8_expected_len;

    /* Bytes the emulator wants written back to the child (cursor position
     * reports for CSI 6n, device attributes for CSI c, ...). The caller
     * should drain this with terminal_grid__take_response() after each
     * terminal_grid__feed() call and forward it to the child's stdin. */
    char response[TERMINAL_ANSI_RESPONSE_CAPACITY];
    size_t response_len;
} terminal_grid_t;

/* Initializes `grid` to a blank columns x rows screen. `cells` and
 * `alt_cells` must each point at caller-owned, zeroed-or-not buffers of
 * columns * rows terminal_cell_t (this clears them). */
void terminal_grid__init(
    terminal_grid_t *grid, terminal_cell_t *cells, terminal_cell_t *alt_cells, uint16_t columns, uint16_t rows
);

/* Feeds raw child-process output through the parser, mutating the grid. */
void terminal_grid__feed(terminal_grid_t *grid, const char *input, size_t input_size);

/* The currently-visible screen (primary or alternate), row-major,
 * columns * rows cells. */
const terminal_cell_t *terminal_grid__active_cells(const terminal_grid_t *grid);

/* Copies out any bytes the emulator wants written back to the child (see
 * `response` above) and clears the pending response. Returns the number of
 * bytes copied (0 if nothing is pending). */
size_t terminal_grid__take_response(terminal_grid_t *grid, char *out, size_t capacity);

/* Resolves a cell's color (TERMINAL_ANSI_COLOR_DEFAULT or a 0-255 xterm
 * 256-color palette index) to an RGB565 display color, using
 * `default_color` for TERMINAL_ANSI_COLOR_DEFAULT. */
uint16_t terminal_ansi__color565(int16_t color, uint16_t default_color);
