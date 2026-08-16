#pragma once

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Font provider interface.                                                   */
/*                                                                            */
/* display_text.c is the font-agnostic renderer: it walks a UTF-8 string,     */
/* asks the active display__font_t for each codepoint's glyph, and blits the  */
/* result. display_font_bitmap.c is the only provider today (a fixed 5-wide   */
/* bitmap font, real per-codepoint glyphs -- see its header comment), but the */
/* seam is here so a second provider (e.g. a FreeType-backed one rasterizing  */
/* TTF/OTF outlines -- see the espressif/freetype component) can be added     */
/* later without changing display_text.c's public API or its callers:        */
/*   1. implement display__font_t.get_glyph() by rasterizing into an         */
/*      display__glyph_t (a coverage-per-pixel path can be added to that     */
/*      struct alongside `columns` when a real antialiased consumer exists;  */
/*      kept out until then rather than carrying an always-unused field);    */
/*   2. point display_internal__set_font() (display_internal.h) at it, or    */
/*      extend display__font_t with a fallback chain if more than one font   */
/*      needs to be active at once (e.g. bitmap font for the ASCII fast path,*/
/*      FreeType for everything it doesn't cover).                          */
/* -------------------------------------------------------------------------- */

/* One glyph column is 16 rows tall (bit0 = top row); DISPLAY_FONT__WIDTH
 * columns make a glyph. Only the built-in font's own cell height (see
 * DISPLAY__FONT_HEIGHT in display_internal.h) is actually used today. */
typedef uint16_t display__column_t;

#define DISPLAY_FONT__WIDTH 5

/* A resolved glyph: pixel data plus the metrics needed to place it. Every
 * field is in un-scaled source pixels -- the renderer applies text_size. */
typedef struct {
    uint8_t width;    /* columns actually used (<= DISPLAY_FONT__WIDTH) */
    uint8_t height;   /* rows in `columns` (<= 16) */
    uint8_t advance;  /* pen advance after this glyph, px */
    /* Column-major, bit0 = row 0 (top). A monospace bitmap font like the
     * built-in one fills every column; a future proportional provider would
     * still report `width` <= DISPLAY_FONT__WIDTH per glyph (this font
     * format has no room for wider glyphs) but can vary `advance`. */
    display__column_t columns[DISPLAY_FONT__WIDTH];
} display__glyph_t;

typedef struct display__font_s {
    const char *name;
    uint8_t cell_width;  /* nominal monospace advance, px (0 if proportional) */
    uint8_t cell_height; /* nominal line-height component, px */
    /* Fills *out_glyph with `codepoint`'s glyph and returns true, or returns
     * false if this font has nothing for it (the renderer then falls back to
     * its own tofu box). Codepoints below 0x20 (control characters) are
     * never passed in -- the renderer handles those itself. */
    bool (*get_glyph)(uint32_t codepoint, display__glyph_t *out_glyph);
} display__font_t;

/* The only provider today: a fixed 5-wide bitmap font with real per-codepoint
 * glyphs for ASCII plus the Latin-1/Latin-Extended-A subset historically
 * supported here (see display_font_bitmap.c for the exact coverage). */
const display__font_t *display_font_bitmap__instance(void);
