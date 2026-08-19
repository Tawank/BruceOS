#include "browser_layout.h"

#define BROWSER_LAYOUT_LINE_GAP 2
#define BROWSER_LAYOUT_PARAGRAPH_GAP 6

int browser_layout__heading_scale(int heading_level) {
    switch (heading_level) {
    case 1: return 4;
    case 2: return 3;
    case 3: return 3;
    default: return 2;
    }
}

int browser_layout__walk(
    const browser_document_t *doc, int width, int char_width, int char_height, browser_layout_visitor_t visitor,
    void *context
) {
    if (doc == NULL || visitor == NULL || width <= 0 || char_width <= 0 || char_height <= 0) return 0;

    int x = 0, y = 0;
    int base_line_height = char_height * 2 + BROWSER_LAYOUT_LINE_GAP;
    /* Height of whatever's on the current (possibly still-open) line, so the
     * trailing "unterminated last line" fix-up below can use the real line
     * height instead of always assuming default-scale text -- see the tail of
     * this function. */
    int current_line_height = base_line_height;

    for (size_t i = 0; i < doc->item_count; ++i) {
        const browser_item_t *item = &doc->items[i];
        switch (item->kind) {
        case BROWSER_ITEM_TEXT: {
            int scale = browser_layout__heading_scale(item->heading_level);
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
                            x = 0;
                            y += line_h;
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
                    x = 0;
                    y += line_h;
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
                current_line_height = line_h;
                x += word_px;
                /* An overlong single word (no spaces to wrap on, e.g. a bare
                 * URL) is drawn once where it lands and may overflow this one
                 * line; the next token still starts fresh. */
                if (x > width) {
                    x = 0;
                    y += line_h;
                }
            }
            break;
        }
        case BROWSER_ITEM_IMAGE: {
            if (x != 0) {
                x = 0;
                y += base_line_height;
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
            break;
        }
        case BROWSER_ITEM_LINE_BREAK:
            x = 0;
            y += base_line_height;
            break;
        case BROWSER_ITEM_PARAGRAPH_BREAK:
            x = 0;
            y += base_line_height + BROWSER_LAYOUT_PARAGRAPH_GAP;
            break;
        }
    }
    /* The document ended mid-line (no trailing break/close event flushed
     * it) -- account for that last open line using its own height, not the
     * default-scale base_line_height, or a page ending on e.g. a heading
     * with no closing tag would report a content height shorter than what
     * actually gets drawn, clipping the bottom of the page when scrolled
     * all the way down. */
    if (x > 0) y += current_line_height;
    return y;
}
