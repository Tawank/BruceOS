#include "terminal_ansi.h"

#include <stdio.h>
#include <string.h>

#define TERMINAL_ANSI_ESCAPE_BYTE 0x1b
#define TERMINAL_ANSI_CSI_FINAL_MIN 0x40
#define TERMINAL_ANSI_CSI_FINAL_MAX 0x7e
#define TERMINAL_ANSI_TAB_WIDTH 8

/* -------------------------------------------------------------------------
 * Color resolution
 * ------------------------------------------------------------------------- */

static const uint16_t terminal_ansi__base16[16] = {
    0x0000, 0x7800, 0x03E0, 0x7BE0, 0x000F, 0x780F, 0x03EF, 0xC618,
    0x7BEF, 0xF800, 0x07E0, 0xFFE0, 0x001F, 0xF81F, 0x07FF, 0xFFFF,
};

uint16_t terminal_ansi__color565(int16_t color, uint16_t default_color) {
    if (color < 0) return default_color;
    if (color < 16) return terminal_ansi__base16[color];
    if (color < 232) {
        static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
        uint16_t idx = (uint16_t)(color - 16);
        uint8_t r = levels[(idx / 36) % 6];
        uint8_t g = levels[(idx / 6) % 6];
        uint8_t b = levels[idx % 6];
        return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    }
    uint8_t gray = (uint8_t)(8 + (color - 232) * 10);
    return (uint16_t)(((gray & 0xf8) << 8) | ((gray & 0xfc) << 3) | (gray >> 3));
}

/* -------------------------------------------------------------------------
 * Grid mutation primitives
 * ------------------------------------------------------------------------- */

static terminal_cell_t *terminal_grid__row(terminal_grid_t *grid, uint16_t y) {
    terminal_cell_t *base = grid->alt_screen_active ? grid->alt_cells : grid->cells;
    return base + (size_t)y * grid->columns;
}

static void terminal_grid__fill_cell(terminal_cell_t *cell, int16_t bg) {
    memset(cell->utf8, 0, sizeof(cell->utf8));
    cell->utf8_len = 0;
    cell->fg = TERMINAL_ANSI_COLOR_DEFAULT;
    cell->bg = bg;
    cell->attrs = 0;
}

static void terminal_grid__erase_line(terminal_grid_t *grid, uint16_t y, uint16_t from, uint16_t to) {
    terminal_cell_t *row = terminal_grid__row(grid, y);
    for (uint16_t x = from; x <= to && x < grid->columns; ++x) terminal_grid__fill_cell(&row[x], grid->bg);
}

static void terminal_grid__clear_buffer(terminal_grid_t *grid, terminal_cell_t *base) {
    for (uint16_t y = 0; y < grid->rows; ++y) {
        terminal_cell_t *row = base + (size_t)y * grid->columns;
        for (uint16_t x = 0; x < grid->columns; ++x) terminal_grid__fill_cell(&row[x], TERMINAL_ANSI_COLOR_DEFAULT);
    }
}

static void terminal_grid__clear_active(terminal_grid_t *grid) {
    terminal_grid__clear_buffer(grid, grid->alt_screen_active ? grid->alt_cells : grid->cells);
}

static void terminal_grid__clamp_cursor(terminal_grid_t *grid) {
    uint16_t min_y = grid->origin_mode ? grid->scroll_top : 0;
    uint16_t max_y = grid->origin_mode ? grid->scroll_bottom : (uint16_t)(grid->rows - 1);
    if (grid->cursor_y < min_y) grid->cursor_y = min_y;
    if (grid->cursor_y > max_y) grid->cursor_y = max_y;
    if (grid->cursor_x >= grid->columns) grid->cursor_x = (uint16_t)(grid->columns - 1);
}

static void terminal_grid__scroll_up(terminal_grid_t *grid, uint16_t n) {
    terminal_cell_t *base = grid->alt_screen_active ? grid->alt_cells : grid->cells;
    uint16_t region_rows = (uint16_t)(grid->scroll_bottom - grid->scroll_top + 1);
    if (n > region_rows) n = region_rows;
    if (n == 0) return;
    for (uint16_t y = grid->scroll_top; (uint16_t)(y + n) <= grid->scroll_bottom; ++y) {
        memcpy(
            base + (size_t)y * grid->columns, base + (size_t)(y + n) * grid->columns,
            sizeof(terminal_cell_t) * grid->columns
        );
    }
    for (uint16_t y = (uint16_t)(grid->scroll_bottom - n + 1); y <= grid->scroll_bottom; ++y) {
        terminal_grid__erase_line(grid, y, 0, (uint16_t)(grid->columns - 1));
    }
}

static void terminal_grid__scroll_down(terminal_grid_t *grid, uint16_t n) {
    terminal_cell_t *base = grid->alt_screen_active ? grid->alt_cells : grid->cells;
    uint16_t region_rows = (uint16_t)(grid->scroll_bottom - grid->scroll_top + 1);
    if (n > region_rows) n = region_rows;
    if (n == 0) return;
    for (uint16_t y = grid->scroll_bottom; y >= (uint16_t)(grid->scroll_top + n); --y) {
        memcpy(
            base + (size_t)y * grid->columns, base + (size_t)(y - n) * grid->columns,
            sizeof(terminal_cell_t) * grid->columns
        );
        if (y == 0) break;
    }
    for (uint16_t y = grid->scroll_top; y < (uint16_t)(grid->scroll_top + n); ++y) {
        terminal_grid__erase_line(grid, y, 0, (uint16_t)(grid->columns - 1));
    }
}

static void terminal_grid__linefeed(terminal_grid_t *grid) {
    grid->pending_wrap = false;
    if (grid->cursor_y == grid->scroll_bottom) {
        terminal_grid__scroll_up(grid, 1);
    } else if ((uint16_t)(grid->cursor_y + 1) < grid->rows) {
        grid->cursor_y++;
    }
}

static void terminal_grid__reverse_index(terminal_grid_t *grid) {
    grid->pending_wrap = false;
    if (grid->cursor_y == grid->scroll_top) {
        terminal_grid__scroll_down(grid, 1);
    } else if (grid->cursor_y > 0) {
        grid->cursor_y--;
    }
}

static void terminal_grid__put_glyph(terminal_grid_t *grid, const uint8_t *bytes, uint8_t len) {
    if (grid->pending_wrap) {
        grid->cursor_x = 0;
        terminal_grid__linefeed(grid);
    }
    terminal_cell_t *row = terminal_grid__row(grid, grid->cursor_y);
    terminal_cell_t *cell = &row[grid->cursor_x];
    memset(cell->utf8, 0, sizeof(cell->utf8));
    memcpy(cell->utf8, bytes, len);
    cell->utf8_len = len;
    cell->fg = grid->fg;
    cell->bg = grid->bg;
    cell->attrs = grid->attrs;
    if ((uint16_t)(grid->cursor_x + 1) >= grid->columns) {
        if (grid->autowrap) grid->pending_wrap = true;
    } else {
        grid->cursor_x++;
    }
}

static void terminal_grid__control(terminal_grid_t *grid, uint8_t byte) {
    switch (byte) {
        case '\r': grid->cursor_x = 0; grid->pending_wrap = false; break;
        case '\n': terminal_grid__linefeed(grid); break;
        case '\b':
            if (grid->cursor_x > 0) grid->cursor_x--;
            grid->pending_wrap = false;
            break;
        case '\t': {
            uint16_t next = (uint16_t)((grid->cursor_x / TERMINAL_ANSI_TAB_WIDTH + 1) * TERMINAL_ANSI_TAB_WIDTH);
            grid->cursor_x = next < grid->columns ? next : (uint16_t)(grid->columns - 1);
            grid->pending_wrap = false;
            break;
        }
        default: break; /* BEL and other C0 controls: no-op */
    }
}

static uint8_t terminal_grid__utf8_len(uint8_t lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1; /* invalid lead byte: treat as a raw single byte */
}

static void terminal_grid__text_byte(terminal_grid_t *grid, uint8_t byte) {
    if (byte < 0x20 || byte == 0x7f) {
        if (grid->utf8_pending_len > 0) {
            terminal_grid__put_glyph(grid, grid->utf8_pending, grid->utf8_pending_len);
            grid->utf8_pending_len = 0;
        }
        terminal_grid__control(grid, byte);
        return;
    }
    if (grid->utf8_pending_len > 0) {
        if ((byte & 0xC0) == 0x80 && grid->utf8_pending_len < grid->utf8_expected_len) {
            grid->utf8_pending[grid->utf8_pending_len++] = byte;
            if (grid->utf8_pending_len == grid->utf8_expected_len) {
                terminal_grid__put_glyph(grid, grid->utf8_pending, grid->utf8_pending_len);
                grid->utf8_pending_len = 0;
            }
            return;
        }
        /* Malformed sequence: flush what was collected, then reprocess this
         * byte as if it started fresh. */
        terminal_grid__put_glyph(grid, grid->utf8_pending, grid->utf8_pending_len);
        grid->utf8_pending_len = 0;
    }
    uint8_t len = terminal_grid__utf8_len(byte);
    if (len == 1) {
        terminal_grid__put_glyph(grid, &byte, 1);
        return;
    }
    grid->utf8_pending[0] = byte;
    grid->utf8_pending_len = 1;
    grid->utf8_expected_len = len;
}

/* -------------------------------------------------------------------------
 * Cursor save/restore and alt-screen switching
 * ------------------------------------------------------------------------- */

static void terminal_grid__save_cursor(terminal_grid_t *grid) {
    grid->saved_cursor_x = grid->cursor_x;
    grid->saved_cursor_y = grid->cursor_y;
    grid->saved_fg = grid->fg;
    grid->saved_bg = grid->bg;
    grid->saved_attrs = grid->attrs;
    grid->saved_origin_mode = grid->origin_mode;
}

static void terminal_grid__restore_cursor(terminal_grid_t *grid) {
    grid->cursor_x = grid->saved_cursor_x;
    grid->cursor_y = grid->saved_cursor_y;
    grid->fg = grid->saved_fg;
    grid->bg = grid->saved_bg;
    grid->attrs = grid->saved_attrs;
    grid->origin_mode = grid->saved_origin_mode;
    grid->pending_wrap = false;
    terminal_grid__clamp_cursor(grid);
}

static void terminal_grid__enter_alt(terminal_grid_t *grid, bool save_cursor) {
    if (grid->alt_screen_active) return;
    if (save_cursor) terminal_grid__save_cursor(grid);
    grid->alt_screen_active = true;
    terminal_grid__clear_active(grid);
    grid->pending_wrap = false;
}

static void terminal_grid__leave_alt(terminal_grid_t *grid, bool restore_cursor) {
    if (!grid->alt_screen_active) return;
    grid->alt_screen_active = false;
    if (restore_cursor) terminal_grid__restore_cursor(grid);
    grid->pending_wrap = false;
}

static void terminal_grid__reset(terminal_grid_t *grid) {
    grid->alt_screen_active = false;
    terminal_grid__clear_buffer(grid, grid->cells);
    terminal_grid__clear_buffer(grid, grid->alt_cells);
    grid->cursor_x = 0;
    grid->cursor_y = 0;
    grid->cursor_visible = true;
    grid->pending_wrap = false;
    grid->scroll_top = 0;
    grid->scroll_bottom = (uint16_t)(grid->rows > 0 ? grid->rows - 1 : 0);
    grid->autowrap = true;
    grid->origin_mode = false;
    grid->fg = TERMINAL_ANSI_COLOR_DEFAULT;
    grid->bg = TERMINAL_ANSI_COLOR_DEFAULT;
    grid->attrs = 0;
}

/* -------------------------------------------------------------------------
 * Editing operations (erase/insert/delete/scroll-region)
 * ------------------------------------------------------------------------- */

static void terminal_grid__erase_in_line(terminal_grid_t *grid, uint16_t mode) {
    if (mode == 0) terminal_grid__erase_line(grid, grid->cursor_y, grid->cursor_x, (uint16_t)(grid->columns - 1));
    else if (mode == 1) terminal_grid__erase_line(grid, grid->cursor_y, 0, grid->cursor_x);
    else terminal_grid__erase_line(grid, grid->cursor_y, 0, (uint16_t)(grid->columns - 1));
}

static void terminal_grid__erase_in_display(terminal_grid_t *grid, uint16_t mode) {
    if (mode == 0) {
        terminal_grid__erase_line(grid, grid->cursor_y, grid->cursor_x, (uint16_t)(grid->columns - 1));
        for (uint16_t y = (uint16_t)(grid->cursor_y + 1); y < grid->rows; ++y) {
            terminal_grid__erase_line(grid, y, 0, (uint16_t)(grid->columns - 1));
        }
    } else if (mode == 1) {
        for (uint16_t y = 0; y < grid->cursor_y; ++y) terminal_grid__erase_line(grid, y, 0, (uint16_t)(grid->columns - 1));
        terminal_grid__erase_line(grid, grid->cursor_y, 0, grid->cursor_x);
    } else {
        for (uint16_t y = 0; y < grid->rows; ++y) terminal_grid__erase_line(grid, y, 0, (uint16_t)(grid->columns - 1));
    }
}

static void terminal_grid__insert_chars(terminal_grid_t *grid, uint16_t n) {
    terminal_cell_t *row = terminal_grid__row(grid, grid->cursor_y);
    uint16_t max_n = (uint16_t)(grid->columns - grid->cursor_x);
    if (n > max_n) n = max_n;
    for (uint16_t x = (uint16_t)(grid->columns - 1); x < grid->columns && x >= (uint16_t)(grid->cursor_x + n); --x) {
        row[x] = row[x - n];
    }
    for (uint16_t x = grid->cursor_x; x < (uint16_t)(grid->cursor_x + n); ++x) terminal_grid__fill_cell(&row[x], grid->bg);
}

static void terminal_grid__delete_chars(terminal_grid_t *grid, uint16_t n) {
    terminal_cell_t *row = terminal_grid__row(grid, grid->cursor_y);
    uint16_t max_n = (uint16_t)(grid->columns - grid->cursor_x);
    if (n > max_n) n = max_n;
    for (uint16_t x = grid->cursor_x; (uint16_t)(x + n) < grid->columns; ++x) row[x] = row[x + n];
    for (uint16_t x = (uint16_t)(grid->columns - n); x < grid->columns; ++x) terminal_grid__fill_cell(&row[x], grid->bg);
}

static void terminal_grid__erase_chars(terminal_grid_t *grid, uint16_t n) {
    terminal_cell_t *row = terminal_grid__row(grid, grid->cursor_y);
    uint16_t end = (uint16_t)(grid->cursor_x + n);
    if (end > grid->columns) end = grid->columns;
    for (uint16_t x = grid->cursor_x; x < end; ++x) terminal_grid__fill_cell(&row[x], grid->bg);
}

static void terminal_grid__insert_lines(terminal_grid_t *grid, uint16_t n) {
    if (grid->cursor_y < grid->scroll_top || grid->cursor_y > grid->scroll_bottom) return;
    uint16_t saved_top = grid->scroll_top;
    grid->scroll_top = grid->cursor_y;
    terminal_grid__scroll_down(grid, n);
    grid->scroll_top = saved_top;
}

static void terminal_grid__delete_lines(terminal_grid_t *grid, uint16_t n) {
    if (grid->cursor_y < grid->scroll_top || grid->cursor_y > grid->scroll_bottom) return;
    uint16_t saved_top = grid->scroll_top;
    grid->scroll_top = grid->cursor_y;
    terminal_grid__scroll_up(grid, n);
    grid->scroll_top = saved_top;
}

static void terminal_grid__set_scroll_region(terminal_grid_t *grid, uint16_t top, uint16_t bottom) {
    if (top >= grid->rows) top = (uint16_t)(grid->rows - 1);
    if (bottom >= grid->rows) bottom = (uint16_t)(grid->rows - 1);
    if (top > bottom) return; /* invalid request: ignore, matches DEC spec */
    grid->scroll_top = top;
    grid->scroll_bottom = bottom;
    grid->cursor_x = 0;
    grid->cursor_y = grid->origin_mode ? top : 0;
    grid->pending_wrap = false;
}

/* -------------------------------------------------------------------------
 * SGR (colors/attributes)
 * ------------------------------------------------------------------------- */

static void terminal_grid__reset_attrs(terminal_grid_t *grid) {
    grid->fg = TERMINAL_ANSI_COLOR_DEFAULT;
    grid->bg = TERMINAL_ANSI_COLOR_DEFAULT;
    grid->attrs = 0;
}

static void terminal_grid__apply_sgr(terminal_grid_t *grid, uint16_t value) {
    if (value == 0) {
        terminal_grid__reset_attrs(grid);
    } else if (value == 1) {
        grid->attrs |= TERMINAL_ANSI_ATTR_BOLD;
        if (grid->fg >= 0 && grid->fg < 8) grid->fg = (int16_t)(grid->fg + 8);
    } else if (value == 2) {
        grid->attrs |= TERMINAL_ANSI_ATTR_DIM;
    } else if (value == 4) {
        grid->attrs |= TERMINAL_ANSI_ATTR_UNDERLINE;
    } else if (value == 7) {
        grid->attrs |= TERMINAL_ANSI_ATTR_REVERSE;
    } else if (value == 22) {
        grid->attrs &= (uint8_t)~(TERMINAL_ANSI_ATTR_BOLD | TERMINAL_ANSI_ATTR_DIM);
        if (grid->fg >= 8 && grid->fg < 16) grid->fg = (int16_t)(grid->fg - 8);
    } else if (value == 24) {
        grid->attrs &= (uint8_t)~TERMINAL_ANSI_ATTR_UNDERLINE;
    } else if (value == 27) {
        grid->attrs &= (uint8_t)~TERMINAL_ANSI_ATTR_REVERSE;
    } else if (value == 39) {
        grid->fg = TERMINAL_ANSI_COLOR_DEFAULT;
    } else if (value == 49) {
        grid->bg = TERMINAL_ANSI_COLOR_DEFAULT;
    } else if (value >= 30 && value <= 37) {
        grid->fg = (int16_t)(value - 30 + ((grid->attrs & TERMINAL_ANSI_ATTR_BOLD) ? 8 : 0));
    } else if (value >= 90 && value <= 97) {
        grid->fg = (int16_t)(value - 90 + 8);
    } else if (value >= 40 && value <= 47) {
        grid->bg = (int16_t)(value - 40);
    } else if (value >= 100 && value <= 107) {
        grid->bg = (int16_t)(value - 100 + 8);
    }
    /* Other SGR codes (italic, blink, strike, ...) are intentionally
     * unsupported: silently ignored rather than breaking the parser. */
}

static void terminal_grid__apply_sgr_params(terminal_grid_t *grid) {
    if (grid->param_count == 0) {
        terminal_grid__reset_attrs(grid);
        return;
    }
    for (uint8_t i = 0; i < grid->param_count; ++i) {
        uint16_t value = grid->params[i];
        if ((value == 38 || value == 48) && (uint8_t)(i + 2) < grid->param_count && grid->params[i + 1] == 5) {
            int16_t color = (int16_t)grid->params[i + 2];
            if (value == 38) grid->fg = color;
            else grid->bg = color;
            i = (uint8_t)(i + 2);
            continue;
        }
        if ((value == 38 || value == 48) && (uint8_t)(i + 1) < grid->param_count && grid->params[i + 1] == 2) {
            /* Truecolor (38/48;2;r;g;b) doesn't fit the 256-color cell
             * palette; skip its subparams instead of misreading them as
             * unrelated SGR codes. */
            uint8_t remaining = (uint8_t)(grid->param_count - i - 1);
            i = (uint8_t)(i + (remaining > 4 ? 4 : remaining));
            continue;
        }
        terminal_grid__apply_sgr(grid, value);
    }
}

/* -------------------------------------------------------------------------
 * Private/DEC mode set-reset (CSI ? ... h / CSI ? ... l)
 * ------------------------------------------------------------------------- */

static void terminal_grid__apply_mode_params(terminal_grid_t *grid, bool enable) {
    if (!grid->csi_private) return; /* non-private modes (IRM, LNM, ...): unsupported no-op */
    for (uint8_t i = 0; i < grid->param_count; ++i) {
        switch (grid->params[i]) {
            case 6:
                grid->origin_mode = enable;
                grid->cursor_x = 0;
                grid->cursor_y = enable ? grid->scroll_top : 0;
                grid->pending_wrap = false;
                break;
            case 7: grid->autowrap = enable; break;
            case 25: grid->cursor_visible = enable; break;
            case 47:
            case 1047:
                if (enable) terminal_grid__enter_alt(grid, false);
                else terminal_grid__leave_alt(grid, false);
                break;
            case 1049:
                if (enable) terminal_grid__enter_alt(grid, true);
                else terminal_grid__leave_alt(grid, true);
                break;
            default: break; /* unsupported private mode: ignore */
        }
    }
}

/* -------------------------------------------------------------------------
 * Responses (device status report / device attributes)
 * ------------------------------------------------------------------------- */

static void terminal_grid__queue_response(terminal_grid_t *grid, const char *text) {
    size_t len = strlen(text);
    if (len >= sizeof(grid->response)) len = sizeof(grid->response) - 1;
    memcpy(grid->response, text, len);
    grid->response[len] = '\0';
    grid->response_len = len;
}

size_t terminal_grid__take_response(terminal_grid_t *grid, char *out, size_t capacity) {
    if (grid->response_len == 0 || capacity == 0) return 0;
    size_t len = grid->response_len < capacity ? grid->response_len : capacity;
    memcpy(out, grid->response, len);
    grid->response_len = 0;
    return len;
}

/* -------------------------------------------------------------------------
 * Parser: parameter helpers
 * ------------------------------------------------------------------------- */

/* A missing param or an explicit 0 both mean "use the default" for count/
 * position parameters (e.g. CSI A with no param, or CSI 0A, both mean 1). */
static uint16_t terminal_grid__param_or(const terminal_grid_t *grid, uint8_t index, uint16_t default_value) {
    if (index >= grid->param_count || grid->params[index] == 0) return default_value;
    return grid->params[index];
}

/* Raw param value, where a missing param and an explicit 0 are the same
 * thing anyway (erase-mode codes: 0 is both "not given" and "to start"). */
static uint16_t terminal_grid__param_raw(const terminal_grid_t *grid, uint8_t index, uint16_t default_value) {
    return index < grid->param_count ? grid->params[index] : default_value;
}

/* -------------------------------------------------------------------------
 * Parser: CSI dispatch
 * ------------------------------------------------------------------------- */

static void terminal_grid__finish_csi(terminal_grid_t *grid, uint8_t final) {
    switch (final) {
        case 'A': {
            int32_t n = terminal_grid__param_or(grid, 0, 1);
            int32_t y = (int32_t)grid->cursor_y - n;
            grid->cursor_y = y < 0 ? 0 : (uint16_t)y;
            grid->pending_wrap = false;
            terminal_grid__clamp_cursor(grid);
            break;
        }
        case 'B': {
            int32_t n = terminal_grid__param_or(grid, 0, 1);
            grid->cursor_y = (uint16_t)(grid->cursor_y + n);
            grid->pending_wrap = false;
            terminal_grid__clamp_cursor(grid);
            break;
        }
        case 'C': {
            int32_t n = terminal_grid__param_or(grid, 0, 1);
            int32_t x = (int32_t)grid->cursor_x + n;
            grid->cursor_x = x >= grid->columns ? (uint16_t)(grid->columns - 1) : (uint16_t)x;
            grid->pending_wrap = false;
            break;
        }
        case 'D': {
            int32_t n = terminal_grid__param_or(grid, 0, 1);
            int32_t x = (int32_t)grid->cursor_x - n;
            grid->cursor_x = x < 0 ? 0 : (uint16_t)x;
            grid->pending_wrap = false;
            break;
        }
        case 'E': {
            int32_t n = terminal_grid__param_or(grid, 0, 1);
            grid->cursor_x = 0;
            grid->cursor_y = (uint16_t)(grid->cursor_y + n);
            grid->pending_wrap = false;
            terminal_grid__clamp_cursor(grid);
            break;
        }
        case 'F': {
            int32_t n = terminal_grid__param_or(grid, 0, 1);
            grid->cursor_x = 0;
            int32_t y = (int32_t)grid->cursor_y - n;
            grid->cursor_y = y < 0 ? 0 : (uint16_t)y;
            grid->pending_wrap = false;
            terminal_grid__clamp_cursor(grid);
            break;
        }
        case 'G': {
            uint16_t col = terminal_grid__param_or(grid, 0, 1);
            grid->cursor_x = (uint16_t)(col - 1) >= grid->columns ? (uint16_t)(grid->columns - 1) : (uint16_t)(col - 1);
            grid->pending_wrap = false;
            break;
        }
        case 'd': {
            uint16_t row = terminal_grid__param_or(grid, 0, 1);
            uint16_t y = (uint16_t)(row - 1);
            grid->cursor_y = y >= grid->rows ? (uint16_t)(grid->rows - 1) : y;
            grid->pending_wrap = false;
            break;
        }
        case 'H':
        case 'f': {
            uint16_t row = terminal_grid__param_or(grid, 0, 1);
            uint16_t col = terminal_grid__param_or(grid, 1, 1);
            uint16_t y = (uint16_t)((grid->origin_mode ? grid->scroll_top : 0) + row - 1);
            uint16_t max_y = grid->origin_mode ? grid->scroll_bottom : (uint16_t)(grid->rows - 1);
            if (y > max_y) y = max_y;
            uint16_t x = (uint16_t)(col - 1);
            if (x >= grid->columns) x = (uint16_t)(grid->columns - 1);
            grid->cursor_y = y;
            grid->cursor_x = x;
            grid->pending_wrap = false;
            break;
        }
        case 'J': terminal_grid__erase_in_display(grid, terminal_grid__param_raw(grid, 0, 0)); break;
        case 'K': terminal_grid__erase_in_line(grid, terminal_grid__param_raw(grid, 0, 0)); break;
        case 'L': terminal_grid__insert_lines(grid, terminal_grid__param_or(grid, 0, 1)); break;
        case 'M': terminal_grid__delete_lines(grid, terminal_grid__param_or(grid, 0, 1)); break;
        case 'P': terminal_grid__delete_chars(grid, terminal_grid__param_or(grid, 0, 1)); break;
        case '@': terminal_grid__insert_chars(grid, terminal_grid__param_or(grid, 0, 1)); break;
        case 'X': terminal_grid__erase_chars(grid, terminal_grid__param_or(grid, 0, 1)); break;
        case 'S': terminal_grid__scroll_up(grid, terminal_grid__param_or(grid, 0, 1)); break;
        case 'T': terminal_grid__scroll_down(grid, terminal_grid__param_or(grid, 0, 1)); break;
        case 'r': {
            uint16_t top = (uint16_t)(terminal_grid__param_or(grid, 0, 1) - 1);
            uint16_t bottom = (uint16_t)(terminal_grid__param_or(grid, 1, grid->rows) - 1);
            terminal_grid__set_scroll_region(grid, top, bottom);
            break;
        }
        case 's':
            grid->saved_cursor_x = grid->cursor_x;
            grid->saved_cursor_y = grid->cursor_y;
            break;
        case 'u':
            grid->cursor_x = grid->saved_cursor_x;
            grid->cursor_y = grid->saved_cursor_y;
            grid->pending_wrap = false;
            break;
        case 'm': terminal_grid__apply_sgr_params(grid); break;
        case 'h': terminal_grid__apply_mode_params(grid, true); break;
        case 'l': terminal_grid__apply_mode_params(grid, false); break;
        case 'n': {
            if (terminal_grid__param_raw(grid, 0, 0) == 6) {
                char buf[24];
                snprintf(
                    buf, sizeof(buf), "\033[%u;%uR", (unsigned)(grid->cursor_y + 1), (unsigned)(grid->cursor_x + 1)
                );
                terminal_grid__queue_response(grid, buf);
            }
            break;
        }
        case 'c':
            if (!grid->csi_private) terminal_grid__queue_response(grid, "\033[?1;0c");
            break;
        default: break; /* unsupported final byte: ignore, matches real parsers */
    }
}

static void terminal_grid__push_param(terminal_grid_t *grid) {
    if (grid->param_count < TERMINAL_ANSI_CSI_MAX_PARAMS) {
        grid->params[grid->param_count++] = grid->has_value ? grid->value : 0;
    }
    grid->value = 0;
    grid->has_value = false;
}

static void terminal_grid__handle_csi_byte(terminal_grid_t *grid, uint8_t byte) {
    if (byte >= '0' && byte <= '9') {
        grid->value = (uint16_t)(grid->value * 10u + (uint16_t)(byte - '0'));
        grid->has_value = true;
    } else if (byte == ';') {
        terminal_grid__push_param(grid);
    } else if (byte == '?' && grid->param_count == 0 && !grid->has_value) {
        grid->csi_private = true;
    } else if (byte >= TERMINAL_ANSI_CSI_FINAL_MIN && byte <= TERMINAL_ANSI_CSI_FINAL_MAX) {
        terminal_grid__push_param(grid);
        terminal_grid__finish_csi(grid, byte);
        grid->state = TERMINAL_ANSI_TEXT;
    }
    /* else: unrecognized intermediate byte (e.g. a space before the final
     * byte) -- skip it, matching how real parsers ignore intermediates they
     * don't model. */
}

static void terminal_grid__handle_escape(terminal_grid_t *grid, uint8_t byte) {
    switch (byte) {
        case '[':
            grid->state = TERMINAL_ANSI_CSI;
            grid->value = 0;
            grid->has_value = false;
            grid->param_count = 0;
            grid->csi_private = false;
            return; /* stay in CSI, don't fall through to TEXT below */
        case ']': grid->state = TERMINAL_ANSI_OSC; return;
        case '7': terminal_grid__save_cursor(grid); break;
        case '8': terminal_grid__restore_cursor(grid); break;
        case 'D': terminal_grid__linefeed(grid); break; /* IND */
        case 'E':
            grid->cursor_x = 0;
            terminal_grid__linefeed(grid); /* NEL */
            break;
        case 'M': terminal_grid__reverse_index(grid); break; /* RI */
        case 'c': terminal_grid__reset(grid); break;         /* RIS */
        default: break;                                      /* unsupported escape: ignore */
    }
    grid->state = TERMINAL_ANSI_TEXT;
}

static void terminal_grid__consume_byte(terminal_grid_t *grid, uint8_t byte) {
    switch (grid->state) {
        case TERMINAL_ANSI_TEXT:
            if (byte == TERMINAL_ANSI_ESCAPE_BYTE) {
                if (grid->utf8_pending_len > 0) {
                    terminal_grid__put_glyph(grid, grid->utf8_pending, grid->utf8_pending_len);
                    grid->utf8_pending_len = 0;
                }
                grid->state = TERMINAL_ANSI_ESCAPE;
            } else {
                terminal_grid__text_byte(grid, byte);
            }
            break;
        case TERMINAL_ANSI_ESCAPE: terminal_grid__handle_escape(grid, byte); break;
        case TERMINAL_ANSI_OSC:
            if (byte == '\a') grid->state = TERMINAL_ANSI_TEXT;
            else if (byte == TERMINAL_ANSI_ESCAPE_BYTE) grid->state = TERMINAL_ANSI_ESCAPE;
            break;
        case TERMINAL_ANSI_CSI: terminal_grid__handle_csi_byte(grid, byte); break;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void terminal_grid__init(
    terminal_grid_t *grid, terminal_cell_t *cells, terminal_cell_t *alt_cells, uint16_t columns, uint16_t rows
) {
    memset(grid, 0, sizeof(*grid));
    grid->cells = cells;
    grid->alt_cells = alt_cells;
    grid->columns = columns;
    grid->rows = rows;
    grid->scroll_top = 0;
    grid->scroll_bottom = (uint16_t)(rows > 0 ? rows - 1 : 0);
    grid->autowrap = true;
    grid->cursor_visible = true;
    grid->fg = TERMINAL_ANSI_COLOR_DEFAULT;
    grid->bg = TERMINAL_ANSI_COLOR_DEFAULT;
    grid->state = TERMINAL_ANSI_TEXT;
    terminal_grid__clear_buffer(grid, cells);
    terminal_grid__clear_buffer(grid, alt_cells);
}

void terminal_grid__feed(terminal_grid_t *grid, const char *input, size_t input_size) {
    for (size_t i = 0; i < input_size; ++i) terminal_grid__consume_byte(grid, (uint8_t)input[i]);
}

const terminal_cell_t *terminal_grid__active_cells(const terminal_grid_t *grid) {
    return grid->alt_screen_active ? grid->alt_cells : grid->cells;
}
