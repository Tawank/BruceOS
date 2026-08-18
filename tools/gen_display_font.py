#!/usr/bin/env python3
"""Regenerates src/core/display/display_font_bitmap.c.

The built-in font's ASCII glyphs and accent shapes are hand-authored pixel
art -- editing them as raw hex column bytes in the .c file is error-prone and
(for accents) the previous approach that did it live at draw time visibly
collided with several accented capitals (see the header comment this script
emits). This script is the source of truth: it draws each ASCII glyph in an
old 8-row cell (rows 0-6 = the letter, row 7 reserved for descenders, exactly
as the classic 5x7 GFX font this project started from), shifts everything
down 2 rows into a 10-row cell to free rows 0-1 for a diacritic, then stamps
each accent's pixels onto that freed headroom (or, for the two accents that
hang below the baseline -- cedilla, ogonek -- onto the shifted-down body).

Run from the repo root: python3 tools/gen_display_font.py
"""
import pathlib

OUT_PATH = pathlib.Path(__file__).resolve().parent.parent / "src/core/display/display_font_bitmap.c"

FONT_WIDTH = 5

# The classic 5x7 GFX-style font, ASCII 32-126, column-major, bit0 = top row
# (row 7 is always blank except where a descender -- g, j, p, q, y, and the
# punctuation that dips below the baseline -- uses it). This is the same
# glyph data the project has used since it adopted this font; kept here
# verbatim as the single source of truth for both the ASCII table and every
# accented glyph derived from it below.
ASCII_5X7 = [
    [0x00, 0x00, 0x00, 0x00, 0x00], [0x00, 0x00, 0x5F, 0x00, 0x00],
    [0x00, 0x07, 0x00, 0x07, 0x00], [0x14, 0x7F, 0x14, 0x7F, 0x14],
    [0x24, 0x2A, 0x7F, 0x2A, 0x12], [0x23, 0x13, 0x08, 0x64, 0x62],
    [0x36, 0x49, 0x56, 0x20, 0x50], [0x00, 0x08, 0x07, 0x03, 0x00],
    [0x00, 0x1C, 0x22, 0x41, 0x00], [0x00, 0x41, 0x22, 0x1C, 0x00],
    [0x2A, 0x1C, 0x7F, 0x1C, 0x2A], [0x08, 0x08, 0x3E, 0x08, 0x08],
    [0x00, 0x80, 0x70, 0x30, 0x00], [0x08, 0x08, 0x08, 0x08, 0x08],
    [0x00, 0x00, 0x60, 0x60, 0x00], [0x20, 0x10, 0x08, 0x04, 0x02],
    [0x3E, 0x51, 0x49, 0x45, 0x3E], [0x00, 0x42, 0x7F, 0x40, 0x00],
    [0x72, 0x49, 0x49, 0x49, 0x46], [0x21, 0x41, 0x49, 0x4D, 0x33],
    [0x18, 0x14, 0x12, 0x7F, 0x10], [0x27, 0x45, 0x45, 0x45, 0x39],
    [0x3C, 0x4A, 0x49, 0x49, 0x31], [0x41, 0x21, 0x11, 0x09, 0x07],
    [0x36, 0x49, 0x49, 0x49, 0x36], [0x46, 0x49, 0x49, 0x29, 0x1E],
    [0x00, 0x00, 0x14, 0x00, 0x00], [0x00, 0x40, 0x34, 0x00, 0x00],
    [0x00, 0x08, 0x14, 0x22, 0x41], [0x14, 0x14, 0x14, 0x14, 0x14],
    [0x00, 0x41, 0x22, 0x14, 0x08], [0x02, 0x01, 0x59, 0x09, 0x06],
    [0x3E, 0x41, 0x5D, 0x59, 0x4E], [0x7C, 0x12, 0x11, 0x12, 0x7C],
    [0x7F, 0x49, 0x49, 0x49, 0x36], [0x3E, 0x41, 0x41, 0x41, 0x22],
    [0x7F, 0x41, 0x41, 0x41, 0x3E], [0x7F, 0x49, 0x49, 0x49, 0x41],
    [0x7F, 0x09, 0x09, 0x09, 0x01], [0x3E, 0x41, 0x41, 0x51, 0x73],
    [0x7F, 0x08, 0x08, 0x08, 0x7F], [0x00, 0x41, 0x7F, 0x41, 0x00],
    [0x20, 0x40, 0x41, 0x3F, 0x01], [0x7F, 0x08, 0x14, 0x22, 0x41],
    [0x7F, 0x40, 0x40, 0x40, 0x40], [0x7F, 0x02, 0x1C, 0x02, 0x7F],
    [0x7F, 0x04, 0x08, 0x10, 0x7F], [0x3E, 0x41, 0x41, 0x41, 0x3E],
    [0x7F, 0x09, 0x09, 0x09, 0x06], [0x3E, 0x41, 0x51, 0x21, 0x5E],
    [0x7F, 0x09, 0x19, 0x29, 0x46], [0x26, 0x49, 0x49, 0x49, 0x32],
    [0x03, 0x01, 0x7F, 0x01, 0x03], [0x3F, 0x40, 0x40, 0x40, 0x3F],
    [0x1F, 0x20, 0x40, 0x20, 0x1F], [0x3F, 0x40, 0x38, 0x40, 0x3F],
    [0x63, 0x14, 0x08, 0x14, 0x63], [0x03, 0x04, 0x78, 0x04, 0x03],
    [0x61, 0x51, 0x49, 0x45, 0x43], [0x00, 0x7F, 0x41, 0x41, 0x41],
    [0x02, 0x04, 0x08, 0x10, 0x20], [0x00, 0x41, 0x41, 0x41, 0x7F],
    [0x04, 0x02, 0x01, 0x02, 0x04], [0x40, 0x40, 0x40, 0x40, 0x40],
    [0x00, 0x03, 0x07, 0x08, 0x00], [0x20, 0x54, 0x54, 0x78, 0x40],
    [0x7F, 0x28, 0x44, 0x44, 0x38], [0x38, 0x44, 0x44, 0x44, 0x28],
    [0x38, 0x44, 0x44, 0x28, 0x7F], [0x38, 0x54, 0x54, 0x54, 0x18],
    [0x00, 0x08, 0x7E, 0x09, 0x02], [0x18, 0xA4, 0xA4, 0x9C, 0x78],
    [0x7F, 0x08, 0x04, 0x04, 0x78], [0x00, 0x44, 0x7D, 0x40, 0x00],
    [0x20, 0x40, 0x40, 0x3D, 0x00], [0x7F, 0x10, 0x28, 0x44, 0x00],
    [0x00, 0x41, 0x7F, 0x40, 0x00], [0x7C, 0x04, 0x78, 0x04, 0x78],
    [0x7C, 0x08, 0x04, 0x04, 0x78], [0x38, 0x44, 0x44, 0x44, 0x38],
    [0xFC, 0x18, 0x24, 0x24, 0x18], [0x18, 0x24, 0x24, 0x18, 0xFC],
    [0x7C, 0x08, 0x04, 0x04, 0x08], [0x48, 0x54, 0x54, 0x54, 0x24],
    [0x04, 0x04, 0x3F, 0x44, 0x24], [0x3C, 0x40, 0x40, 0x20, 0x7C],
    [0x1C, 0x20, 0x40, 0x20, 0x1C], [0x3C, 0x40, 0x30, 0x40, 0x3C],
    [0x44, 0x28, 0x10, 0x28, 0x44], [0x4C, 0x90, 0x90, 0x90, 0x7C],
    [0x44, 0x64, 0x54, 0x4C, 0x44], [0x00, 0x08, 0x36, 0x41, 0x00],
    [0x00, 0x00, 0x77, 0x00, 0x00], [0x00, 0x41, 0x36, 0x08, 0x00],
    [0x02, 0x01, 0x02, 0x04, 0x02],
]
assert len(ASCII_5X7) == 95

DOTLESS_I_5X7 = [0x00, 0x44, 0x7C, 0x40, 0x00]


def shift(col_bytes):
    """Moves an 8-row-cell glyph's ink down 2 rows into the 10-row cell,
    freeing rows 0-1 for a diacritic."""
    return [v << 2 for v in col_bytes]


ASCII_SHIFTED = [shift(g) for g in ASCII_5X7]
DOTLESS_I_SHIFTED = shift(DOTLESS_I_5X7)

# Accent pixel positions in (col, row) units of the 10-row cell -- the same
# shapes display__draw_accent() used to stamp on at draw time, just re-based:
# "headroom" marks keep their old row (0 or 1, now genuinely empty); the two
# marks that hang below the letter (cedilla, ogonek) shift down 2 with the
# body instead, same as the letter itself.
ACCENTS = {
    "ACUTE": [(3, 0), (2, 1)],
    "GRAVE": [(1, 0), (2, 1)],
    "CIRCUMFLEX": [(2, 0), (1, 1), (2, 1), (3, 1)],
    "TILDE": [(1, 0), (2, 0), (3, 1)],
    "DIAERESIS": [(1, 0), (3, 0)],
    "CARON": [(1, 0), (3, 0), (2, 1)],
    "BREVE": [(1, 0), (3, 0), (2, 1)],
    "RING": [(1, 0), (1, 1), (3, 0), (3, 1)],
    "DOUBLE_ACUTE": [(2, 0), (4, 0), (1, 1), (3, 1)],
    "DOT_ABOVE": [(2, 0)],
    "APOSTROPHE": [(4, 0), (3, 1)],
    "CEDILLA": [(2, 9)],
    "OGONEK": [(3, 9), (4, 9)],
    "STROKE": [(0, 5), (1, 5), (2, 5), (3, 5), (4, 5)],
}

# Accents that float above the letter in the reserved rows 0-1 headroom
# (everything except the two marks that attach below the baseline -- cedilla,
# ogonek -- or through the body -- stroke -- which move with the letter
# instead). A lowercase x-height letter's own ink only starts at row 4 (its
# ascender-free old glyph started 2 rows lower than a capital's, before the
# +2 shift), so pinning these to rows 0-1 regardless of case leaves a
# floating 2-row gap on lowercase letters that capitals don't have -- the
# accent reads as detached instead of sitting on the letter. Dropping it by
# 2 rows for a lowercase base puts it flush against the x-height top, same
# as it already sits flush against a capital's cap-height top.
#
# APOSTROPHE is the one exception: it is only ever used on lowercase d/t/l
# (d-caron/t-caron/l-caron, "dj"/"tj"/"lj"-style Czech/Slovak soft marks),
# whose ascenders reach all the way to row 2 just like a capital -- there is
# no gap to close, and dropping it 2 rows would run it straight into the
# ascender's own ink.
HEADROOM_ACCENTS = {
    "ACUTE", "GRAVE", "CIRCUMFLEX", "TILDE", "DIAERESIS", "CARON", "BREVE",
    "RING", "DOUBLE_ACUTE", "DOT_ABOVE",
}


def base_glyph(letter, dotless=False):
    return list(DOTLESS_I_SHIFTED if dotless else ASCII_SHIFTED[ord(letter) - 32])


def with_accent(letter, accent, dotless=False):
    g = base_glyph(letter, dotless)
    row_offset = 2 if accent in HEADROOM_ACCENTS and letter.islower() else 0
    for col, row in ACCENTS[accent]:
        g[col] |= 1 << (row + row_offset)
    return g


# codepoint -> (base_letter, accent_or_None, dotless_body)
MAP = {}


def add(cp, letter, accent=None, dotless=False):
    MAP[cp] = (letter, accent, dotless)


SAME_ACCENT_PAIRS = [
    (0x00C0, "A", 0x00E0, "a", "GRAVE"), (0x00C1, "A", 0x00E1, "a", "ACUTE"),
    (0x00C2, "A", 0x00E2, "a", "CIRCUMFLEX"), (0x00C3, "A", 0x00E3, "a", "TILDE"),
    (0x00C4, "A", 0x00E4, "a", "DIAERESIS"), (0x00C7, "C", 0x00E7, "c", "CEDILLA"),
    (0x00C8, "E", 0x00E8, "e", "GRAVE"), (0x00C9, "E", 0x00E9, "e", "ACUTE"),
    (0x00CA, "E", 0x00EA, "e", "CIRCUMFLEX"), (0x00CB, "E", 0x00EB, "e", "DIAERESIS"),
    (0x00D1, "N", 0x00F1, "n", "TILDE"), (0x00D2, "O", 0x00F2, "o", "GRAVE"),
    (0x00D3, "O", 0x00F3, "o", "ACUTE"), (0x00D4, "O", 0x00F4, "o", "CIRCUMFLEX"),
    (0x00D5, "O", 0x00F5, "o", "TILDE"), (0x00D6, "O", 0x00F6, "o", "DIAERESIS"),
    (0x00D9, "U", 0x00F9, "u", "GRAVE"), (0x00DA, "U", 0x00FA, "u", "ACUTE"),
    (0x00DB, "U", 0x00FB, "u", "CIRCUMFLEX"), (0x00DC, "U", 0x00FC, "u", "DIAERESIS"),
    (0x0102, "A", 0x0103, "a", "BREVE"), (0x0104, "A", 0x0105, "a", "OGONEK"),
    (0x0106, "C", 0x0107, "c", "ACUTE"), (0x010C, "C", 0x010D, "c", "CARON"),
    (0x0160, "S", 0x0161, "s", "CARON"), (0x017D, "Z", 0x017E, "z", "CARON"),
    (0x0110, "D", 0x0111, "d", "STROKE"), (0x0118, "E", 0x0119, "e", "OGONEK"),
    (0x011A, "E", 0x011B, "e", "CARON"), (0x011E, "G", 0x011F, "g", "BREVE"),
    (0x0141, "L", 0x0142, "l", "STROKE"), (0x0143, "N", 0x0144, "n", "ACUTE"),
    (0x0147, "N", 0x0148, "n", "CARON"), (0x0150, "O", 0x0151, "o", "DOUBLE_ACUTE"),
    (0x0154, "R", 0x0155, "r", "ACUTE"), (0x0158, "R", 0x0159, "r", "CARON"),
    (0x015E, "S", 0x015F, "s", "CEDILLA"), (0x016E, "U", 0x016F, "u", "RING"),
    (0x015A, "S", 0x015B, "s", "ACUTE"), (0x0179, "Z", 0x017A, "z", "ACUTE"),
    (0x017B, "Z", 0x017C, "z", "DOT_ABOVE"), (0x0218, "S", 0x0219, "s", "CEDILLA"),
    (0x021A, "T", 0x021B, "t", "CEDILLA"), (0x0139, "L", 0x013A, "l", "ACUTE"),
]
for upper_cp, upper_l, lower_cp, lower_l, acc in SAME_ACCENT_PAIRS:
    add(upper_cp, upper_l, acc)
    add(lower_cp, lower_l, acc)

add(0x00DD, "Y", "ACUTE")
add(0x00FD, "y", "ACUTE")
add(0x010E, "D", "CARON")
add(0x010F, "d", "APOSTROPHE")
add(0x0164, "T", "CARON")
add(0x0165, "t", "APOSTROPHE")
add(0x013D, "L", "CARON")
add(0x013E, "l", "APOSTROPHE")
add(0x0131, "i", None, dotless=True)  # Turkish dotless i, no accent

# Accented i: the tittle (dot) is conventionally dropped when a diacritic is
# added, so the lowercase forms use the dotless body. Uppercase I has no
# tittle to begin with, so it keeps the normal glyph.
for cp, acc in [(0x00CC, "GRAVE"), (0x00CD, "ACUTE"), (0x00CE, "CIRCUMFLEX"), (0x00CF, "DIAERESIS")]:
    add(cp, "I", acc)
for cp, acc in [(0x00EC, "GRAVE"), (0x00ED, "ACUTE"), (0x00EE, "CIRCUMFLEX"), (0x00EF, "DIAERESIS")]:
    add(cp, "i", acc, dotless=True)

# Codepoints that render identically to a plain ASCII glyph -- no dedicated
# bitmap needed, just point at the existing one.
ALIASES = {0x2018: "'", 0x2019: "'", 0x201C: '"', 0x201D: '"'}

# Hand-authored pixel art for glyphs that don't derive from a Latin base
# letter: box-drawing lines/corners, block elements, and the status/spinner
# marks TUI apps (Claude Code, opencode, and the CLI-spinner libraries they're
# built on) print routinely. Each value is already a final 5-column x10-row
# bitmap (bit0 = row 0/top), not run through the accent-shift pipeline above.
# A horizontal box rule sits on row 4 (center); a vertical one spans the full
# cell in the middle column; corners meet them there. The four rounded
# corners (U+256D-U+2570) leave that meeting pixel empty and instead touch
# diagonally one row/column off, which reads as a curve rather than a right
# angle at this resolution without needing a distinct antialiased shape.
RAW_GLYPHS = {
    # Box drawing: light lines, square corners, tees, cross.
    0x2500: (0x0010, 0x0010, 0x0010, 0x0010, 0x0010),  # ─
    0x2502: (0x0000, 0x0000, 0x03FF, 0x0000, 0x0000),  # │
    0x250C: (0x0000, 0x0000, 0x03F0, 0x0010, 0x0010),  # ┌
    0x2510: (0x0010, 0x0010, 0x03F0, 0x0000, 0x0000),  # ┐
    0x2514: (0x0000, 0x0000, 0x001F, 0x0010, 0x0010),  # └
    0x2518: (0x0010, 0x0010, 0x001F, 0x0000, 0x0000),  # ┘
    0x251C: (0x0000, 0x0000, 0x03FF, 0x0010, 0x0010),  # ├
    0x2524: (0x0010, 0x0010, 0x03FF, 0x0000, 0x0000),  # ┤
    0x252C: (0x0010, 0x0010, 0x03F0, 0x0010, 0x0010),  # ┬
    0x2534: (0x0010, 0x0010, 0x001F, 0x0010, 0x0010),  # ┴
    0x253C: (0x0010, 0x0010, 0x03FF, 0x0010, 0x0010),  # ┼
    # Box drawing: light rounded corners (Claude Code / opencode panel borders).
    0x256D: (0x0000, 0x0000, 0x03E0, 0x0010, 0x0010),  # ╭
    0x256E: (0x0010, 0x0010, 0x03E0, 0x0000, 0x0000),  # ╮
    0x2570: (0x0000, 0x0000, 0x000F, 0x0010, 0x0010),  # ╰
    0x256F: (0x0010, 0x0010, 0x000F, 0x0000, 0x0000),  # ╯
    # Block elements.
    0x2580: (0x001F, 0x001F, 0x001F, 0x001F, 0x001F),  # ▀ upper half block
    0x2584: (0x03E0, 0x03E0, 0x03E0, 0x03E0, 0x03E0),  # ▄ lower half block
    0x2588: (0x03FF, 0x03FF, 0x03FF, 0x03FF, 0x03FF),  # █ full block
    0x258C: (0x03FF, 0x03FF, 0x0000, 0x0000, 0x0000),  # ▌ left half block
    0x2590: (0x0000, 0x0000, 0x0000, 0x03FF, 0x03FF),  # ▐ right half block
    0x2591: (0x0092, 0x0249, 0x0124, 0x0092, 0x0249),  # ░ light shade (~25%)
    0x2592: (0x0155, 0x02AA, 0x0155, 0x02AA, 0x0155),  # ▒ medium shade (50%)
    0x2593: (0x036D, 0x01B6, 0x02DB, 0x036D, 0x01B6),  # ▓ dark shade (~75%)
    0x259B: (0x03FF, 0x03FF, 0x03FF, 0x001F, 0x001F),  # ▛ quadrant UL+UR+LL
    # Status / navigation marks.
    0x2022: (0x0000, 0x0030, 0x0030, 0x0030, 0x0000),  # • bullet
    0x203B: (0x0022, 0x0094, 0x00C8, 0x0094, 0x0022),  # ※ reference mark
    0x2192: (0x0020, 0x0020, 0x00A8, 0x0070, 0x0020),  # → rightwards arrow
    0x2190: (0x0020, 0x0070, 0x00A8, 0x0020, 0x0020),  # ← leftwards arrow
    0x2026: (0x0180, 0x0000, 0x0180, 0x0000, 0x0180),  # … horizontal ellipsis
    0x2013: (0x0000, 0x0010, 0x0010, 0x0010, 0x0000),  # – en dash
    0x2014: (0x0010, 0x0010, 0x0010, 0x0010, 0x0010),  # — em dash
    0x2713: (0x0030, 0x0040, 0x0040, 0x0020, 0x001C),  # ✓ check mark
    0x2717: (0x0084, 0x0048, 0x0030, 0x0048, 0x0084),  # ✗ ballot X
    0x2722: (0x0030, 0x0030, 0x00FC, 0x0030, 0x0030),  # ✢ four-spoked asterisk
    0x25CB: (0x0078, 0x0084, 0x0084, 0x0084, 0x0078),  # ○ white circle
    0x25CF: (0x0078, 0x00FC, 0x00FC, 0x00FC, 0x0078),  # ● black circle (bullet)
    0x23F5: (0x01FE, 0x00FC, 0x0078, 0x0030, 0x0000),  # ⏵ play triangle
    0x23F8: (0x01FE, 0x0000, 0x0000, 0x01FE, 0x0000),  # ⏸ pause bars
    0x23F9: (0x00FC, 0x00FC, 0x00FC, 0x00FC, 0x0000),  # ⏹ stop square
}

# Eight-spoked star glyphs used interchangeably as spinner animation frames
# (e.g. Claude Code's "thinking" indicator cycles through several of these) --
# indistinguishable from each other at 5x10, so they share one bitmap rather
# than pretending to be visually distinct shapes.
_EIGHT_SPOKE_ASTERISK = (0x0231, 0x0132, 0x00FC, 0x0132, 0x0231)  # ✻ etc.
for _cp in (0x2733, 0x2734, 0x2736, 0x273B, 0x273D):
    RAW_GLYPHS[_cp] = _EIGHT_SPOKE_ASTERISK

# Braille patterns U+2800-28FF: mechanically derived from the standard 8-dot
# bit layout (bit0=dot1, bit1=dot2, ..., bit7=dot8; dots 1-3 and 7 in the left
# column top-to-bottom, dots 4-6 and 8 in the right column), not hand-drawn.
# Widely used by CLI spinner libraries (e.g. cli-spinners' "dots", which
# several TUI tools default to) as well as literal braille text.
_BRAILLE_DOTS = [(1, 2), (1, 4), (1, 6), (3, 2), (3, 4), (3, 6), (1, 8), (3, 8)]
for _n in range(256):
    _cols = [0, 0, 0, 0, 0]
    for _bit, (_col, _row) in enumerate(_BRAILLE_DOTS):
        if _n & (1 << _bit):
            _cols[_col] |= 1 << _row
    RAW_GLYPHS[0x2800 + _n] = tuple(_cols)


def c_char_literal(ch):
    if ch == "'":
        return "'\\''"
    if ch == "\\":
        return "'\\\\'"
    return f"'{ch}'"


def emit():
    lines = []
    w = lines.append
    w("#include \"display_font.h\"")
    w("")
    w("#include \"display_internal.h\"")
    w("")
    w("/* -------------------------------------------------------------------------- */")
    w("/* Built-in font: fixed 5-wide bitmap glyphs, ASCII 32-126 plus the Latin-1   */")
    w("/* Supplement / Latin Extended-A subset this project has historically needed */")
    w("/* (Western/Central European accented letters, common quote glyphs), plus    */")
    w("/* box-drawing/block/status glyphs and the full Braille block (U+2800-28FF)  */")
    w("/* -- the symbols TUI tools like Claude Code and opencode print for panel    */")
    w("/* borders, progress spinners, and status marks.                            */")
    w("/*                                                                            */")
    w("/* Earlier revisions rendered an accented letter by drawing the plain ASCII  */")
    w("/* base glyph and stamping a hardcoded accent shape on top of it at draw     */")
    w("/* time. That collided with the base glyph's own ink for most accented      */")
    w("/* capitals -- e.g. E/O/C/S/Z/I fill row 0 solid, so an acute/grave/caron/   */")
    w("/* diaeresis drawn there just disappeared into (or smudged) the letter's top */")
    w("/* stroke instead of reading as a separate mark.                             */")
    w("/*                                                                            */")
    w("/* Every glyph here is a real, static per-codepoint bitmap instead: rows 0-1 */")
    w("/* of the cell are reserved (every s_ascii/s_extended entry has them blank)  */")
    w("/* so a diacritic always has genuine headroom, the same way lowercase        */")
    w("/* ascenders already did in the old font (x-height letters left rows 0-1     */")
    w("/* blank; capitals didn't). That makes a collision structurally impossible   */")
    w("/* rather than something to hand-verify per accent/letter combination.       */")
    w("/*                                                                            */")
    w("/* Generated by tools/gen_display_font.py -- regenerate with that script     */")
    w("/* rather than hand-editing the tables below. */")
    w("/* -------------------------------------------------------------------------- */")
    w("")
    w("typedef struct {")
    w("    uint16_t codepoint;")
    w("    display__column_t columns[DISPLAY_FONT__WIDTH];")
    w("} display__extended_glyph_t;")
    w("")
    w("typedef struct {")
    w("    uint16_t codepoint;")
    w("    char ascii;")
    w("} display__alias_glyph_t;")
    w("")
    w("/* ASCII 32-126, dense, index = codepoint - 32. Column-major, bit0 = row 0")
    w(" * (top). */")
    w("static const display__column_t s_ascii[95][DISPLAY_FONT__WIDTH] = {")
    for g in ASCII_SHIFTED:
        w("    {" + ", ".join(f"0x{v:04X}" for v in g) + "},")
    w("};")
    w("")
    assert not (set(MAP) & set(RAW_GLYPHS)), "codepoint defined in both MAP and RAW_GLYPHS"
    extended = {}
    for cp, (letter, accent, dotless) in MAP.items():
        extended[cp] = list(with_accent(letter, accent, dotless) if accent else base_glyph(letter, dotless))
    for cp, columns in RAW_GLYPHS.items():
        extended[cp] = list(columns)

    w("/* Non-ASCII glyphs, real per-codepoint bitmaps (not synthesized at draw time):")
    w(" * accented Latin letters composed with their diacritic (see MAP above), plus")
    w(" * hand-authored/algorithmic symbol and Braille glyphs (see RAW_GLYPHS above).")
    w(" * Sorted by codepoint for binary search. */")
    w("static const display__extended_glyph_t s_extended[] = {")
    for cp in sorted(extended.keys()):
        g = extended[cp]
        w(f"    {{0x{cp:04X}, {{{', '.join(f'0x{v:04X}' for v in g)}}}}}, /* U+{cp:04X} */")
    w("};")
    w("")
    w("/* Codepoints that render identically to an existing ASCII glyph (typographic")
    w(" * quote substitutes) -- no separate bitmap needed. */")
    w("static const display__alias_glyph_t s_aliases[] = {")
    for cp in sorted(ALIASES.keys()):
        w(f"    {{0x{cp:04X}, {c_char_literal(ALIASES[cp])}}},")
    w("};")
    w("")
    w("static void display_font_bitmap__fill(display__glyph_t *out_glyph, const display__column_t *columns) {")
    w("    out_glyph->width = DISPLAY_FONT__WIDTH;")
    w("    out_glyph->height = DISPLAY__FONT_HEIGHT + 1;")
    w("    out_glyph->advance = DISPLAY__FONT_WIDTH + 1;")
    w("    for (uint8_t col = 0; col < DISPLAY_FONT__WIDTH; ++col) { out_glyph->columns[col] = columns[col]; }")
    w("}")
    w("")
    w("static bool display_font_bitmap__get_glyph(uint32_t codepoint, display__glyph_t *out_glyph) {")
    w("    if (codepoint >= DISPLAY__FONT_FIRST && codepoint <= DISPLAY__FONT_LAST) {")
    w("        display_font_bitmap__fill(out_glyph, s_ascii[codepoint - DISPLAY__FONT_FIRST]);")
    w("        return true;")
    w("    }")
    w("")
    w("    /* s_extended[] is sorted by codepoint; binary search it. */")
    w("    size_t lo = 0, hi = sizeof(s_extended) / sizeof(s_extended[0]);")
    w("    while (lo < hi) {")
    w("        size_t mid = lo + (hi - lo) / 2;")
    w("        if (s_extended[mid].codepoint == codepoint) {")
    w("            display_font_bitmap__fill(out_glyph, s_extended[mid].columns);")
    w("            return true;")
    w("        }")
    w("        if (s_extended[mid].codepoint < codepoint) lo = mid + 1;")
    w("        else hi = mid;")
    w("    }")
    w("")
    w("    for (size_t i = 0; i < sizeof(s_aliases) / sizeof(s_aliases[0]); ++i) {")
    w("        if (s_aliases[i].codepoint == codepoint) {")
    w("            return display_font_bitmap__get_glyph((uint32_t)(unsigned char)s_aliases[i].ascii, out_glyph);")
    w("        }")
    w("    }")
    w("")
    w("    return false;")
    w("}")
    w("")
    w("const display__font_t *display_font_bitmap__instance(void) {")
    w("    static const display__font_t s_font = {")
    w('        .name = "bruce-5x10",')
    w("        .cell_width = DISPLAY__FONT_WIDTH + 1,")
    w("        .cell_height = DISPLAY__FONT_HEIGHT + 1,")
    w("        .get_glyph = display_font_bitmap__get_glyph,")
    w("    };")
    w("    return &s_font;")
    w("}")
    w("")
    return "\n".join(lines)


if __name__ == "__main__":
    OUT_PATH.write_text(emit())
    print(f"wrote {OUT_PATH} ({len(MAP) + len(RAW_GLYPHS)} extended glyphs, {len(ALIASES)} aliases)")
