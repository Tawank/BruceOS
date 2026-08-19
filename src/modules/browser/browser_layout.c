#include "browser_layout.h"

#define BROWSER_LAYOUT_LINE_GAP 2
#define BROWSER_LAYOUT_PARAGRAPH_GAP 6

int browser_layout__heading_scale(int heading_level, int font_scale_delta) {
    int base;
    switch (heading_level) {
    case 1: base = 4; break;
    case 2: base = 3; break;
    case 3: base = 3; break;
    default: base = 2; break;
    }
    int scale = base + font_scale_delta;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8; /* display__set_text_size() takes a uint8_t; keep it off the wall too. */
    return scale;
}

int browser_layout__walk(
    const browser_document_t *doc, int width, int char_width, int char_height, int font_scale_delta,
    browser_layout_visitor_t visitor, void *context
) {
    if (doc == NULL || visitor == NULL || width <= 0 || char_width <= 0 || char_height <= 0) return 0;

    int x = 0, y = 0;
    int base_line_height = char_height * browser_layout__heading_scale(0, font_scale_delta) + BROWSER_LAYOUT_LINE_GAP;
    /* Tallest line_height of any token placed on the current row so far (0 =
     * nothing placed yet this row). A row isn't always one item -- e.g. a
     * heading item is immediately followed by plain-text item with no break
     * between them (HEADING_END emits none) -- so the row's real height can
     * exceed the line_h of whichever item happens to trigger the wrap off of
     * it. Advancing y by just that item's own line_h (or the constant
     * default-scale base_line_height, for a break) instead of this tracked
     * max was under-counting the row whenever a taller item (a heading) sat
     * next to a shorter one on the same row: the next row would start before
     * the taller item's ink actually ended, so it visibly overlapped/covered
     * the row below it. */
    int row_height = 0;

    for (size_t i = 0; i < doc->item_count; ++i) {
        const browser_item_t *item = &doc->items[i];
        switch (item->kind) {
        case BROWSER_ITEM_TEXT: {
            int scale = browser_layout__heading_scale(item->heading_level, font_scale_delta);
            int line_h = char_height * scale + BROWSER_LAYOUT_LINE_GAP;
            const char *text = doc->text_pool + item->text_offset;
            size_t len = item->text_len;
            size_t k = 0;
            while (k < len) {
                if (text[k] == ' ') {
                    int space_px = char_width * scale;
                    if (x > 0) {
                        if (x + space_px <= width) x += space_px;
                        else {
                            y += row_height > 0 ? row_height : line_h;
                            x = 0;
                            row_height = 0;
                        }
                    }
                    k++;
                    continue;
                }
                size_t start = k;
                while (k < len && text[k] != ' ') k++;
                size_t word_len = k - start;
                int word_px = (int)word_len * char_width * scale;
                if (x > 0 && x + word_px > width) {
                    y += row_height > 0 ? row_height : line_h;
                    x = 0;
                    row_height = 0;
                }
                browser_layout_token_t token = {
                    .x = x,
                    .y = y,
                    .line_height = line_h,
                    .text = text + start,
                    .text_len = word_len,
                    .heading_level = item->heading_level,
                    .link_index = item->link_index,
                    .image_index = -1,
                };
                visitor(&token, context);
                if (line_h > row_height) row_height = line_h;
                x += word_px;
                /* An overlong single word (no spaces to wrap on, e.g. a bare
                 * URL) is drawn once where it lands and may overflow this one
                 * line; the next token still starts fresh. */
                if (x > width) {
                    y += row_height;
                    x = 0;
                    row_height = 0;
                }
            }
            break;
        }
        case BROWSER_ITEM_IMAGE: {
            if (x != 0) {
                y += row_height > 0 ? row_height : base_line_height;
                x = 0;
                row_height = 0;
            }
            browser_layout_token_t token = {
                .x = 0,
                .y = y,
                .line_height = BROWSER_IMAGE_BOX_HEIGHT,
                .text = NULL,
                .text_len = 0,
                .heading_level = 0,
                .link_index = -1,
                .image_index = item->image_index,
            };
            visitor(&token, context);
            y += BROWSER_IMAGE_BOX_HEIGHT;
            x = 0;
            row_height = 0;
            break;
        }
        case BROWSER_ITEM_LINE_BREAK:
            y += row_height > 0 ? row_height : base_line_height;
            x = 0;
            row_height = 0;
            break;
        case BROWSER_ITEM_PARAGRAPH_BREAK:
            y += (row_height > 0 ? row_height : base_line_height) + BROWSER_LAYOUT_PARAGRAPH_GAP;
            x = 0;
            row_height = 0;
            break;
        }
    }
    /* The document ended mid-line (no trailing break/close event flushed
     * it) -- account for that last open row using its real tracked height,
     * not the default-scale base_line_height, or a page ending on e.g. a
     * heading with no closing tag would report a content height shorter than
     * what actually gets drawn, clipping the bottom of the page when
     * scrolled all the way down. */
    if (x > 0) y += row_height > 0 ? row_height : base_line_height;
    return y;
}
