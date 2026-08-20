#include "browser_render.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "browser_layout.h"
#include "core_sdk/config.h"
#include "core_sdk/display.h"

#define BROWSER_CONTENT_MARGIN 2
#define BROWSER_CHROME_PADDING 5
#define BROWSER_WORD_BUF_MAX 64

typedef struct {
    bruce_display_color_t primary;
    bruce_display_color_t secondary;
    bruce_display_color_t background;
    bruce_display_color_t surface;
    bruce_display_color_t text;
    bruce_display_color_t text_muted;
    bruce_display_color_t border;
} browser_render__theme_t;

static void browser_render__get_theme(browser_render__theme_t *theme) {
    theme->primary = config__get_color_primary();
    theme->secondary = config__get_color_secondary();
    theme->background = config__get_color_background();
    theme->surface = config__get_color_surface();
    theme->text = config__get_color_text();
    theme->text_muted = config__get_color_text_muted();
    theme->border = config__get_color_border();
}

static void browser_render__metrics(int16_t *char_width, int16_t *char_height) {
    if (display__get_font_metrics(char_width, char_height) != BRUCE_OK || *char_width <= 0 || *char_height <= 0) {
        *char_width = 6;
        *char_height = 8;
    }
}

int browser_render__chrome_height(void) {
    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    return char_height + 2 * BROWSER_CHROME_PADDING;
}

int browser_render__content_width(void) {
    int width = display__width() - 2 * BROWSER_CONTENT_MARGIN;
    return width > 0 ? width : 0;
}

int browser_render__view_height(void) {
    int height = display__height() - browser_render__chrome_height();
    return height > 0 ? height : 0;
}

static void browser_render__noop_visitor(const browser_layout_token_t *token, void *context) {
    (void)token;
    (void)context;
}

int browser_render__content_height(const browser_document_t *doc, int font_scale) {
    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    return browser_layout__walk(
        doc, browser_render__content_width(), char_width, char_height, font_scale, browser_render__noop_visitor, NULL
    );
}

int browser_render__max_scroll(const browser_document_t *doc, int font_scale) {
    int max_scroll = browser_render__content_height(doc, font_scale) - browser_render__view_height();
    return max_scroll > 0 ? max_scroll : 0;
}

typedef struct {
    size_t item_index;
    int y;
    bool found;
} browser_render__item_search_t;

static void browser_render__item_visitor(const browser_layout_token_t *token, void *context) {
    browser_render__item_search_t *search = context;
    if (!search->found && token->item_index >= search->item_index) {
        search->y = token->y;
        search->found = true;
    }
}

int browser_render__item_y(const browser_document_t *doc, size_t item_index, int font_scale) {
    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    browser_render__item_search_t search = {.item_index = item_index};
    int height = browser_layout__walk(
        doc, browser_render__content_width(), char_width, char_height, font_scale, browser_render__item_visitor,
        &search
    );
    return search.found ? search.y : height;
}

typedef struct {
    int wanted_index;
    int reference_index;
    int edge_y;
    int direction;
    bool direct;
    bool from_edge;
    bool found;
    browser_render_link_bounds_t bounds;
} browser_render__link_search_t;

static void browser_render__link_visitor(const browser_layout_token_t *token, void *context) {
    browser_render__link_search_t *search = context;
    int index = token->link_index;
    if (index < 0) return;

    bool qualifies;
    if (search->direct) qualifies = index == search->wanted_index;
    else if (search->from_edge) {
        qualifies = search->direction > 0 ? token->y + token->line_height > search->edge_y
                                          : token->y < search->edge_y;
    } else {
        qualifies = search->direction > 0 ? index > search->reference_index : index < search->reference_index;
    }
    if (!qualifies) return;

    bool better = !search->found || (search->direction > 0 && index < search->bounds.link_index) ||
                  (search->direction < 0 && index > search->bounds.link_index);
    if (better) {
        search->bounds = (browser_render_link_bounds_t){
            .link_index = index, .top = token->y, .bottom = token->y + token->line_height
        };
        search->found = true;
    } else if (index == search->bounds.link_index) {
        if (token->y < search->bounds.top) search->bounds.top = token->y;
        int bottom = token->y + token->line_height;
        if (bottom > search->bounds.bottom) search->bounds.bottom = bottom;
    }
}

static bool browser_render__run_link_search(
    const browser_document_t *doc, int font_scale, browser_render__link_search_t *search,
    browser_render_link_bounds_t *out_bounds
) {
    if (doc == NULL || search == NULL || out_bounds == NULL) return false;
    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    (void)browser_layout__walk(
        doc, browser_render__content_width(), char_width, char_height, font_scale, browser_render__link_visitor,
        search
    );
    if (!search->found) return false;
    *out_bounds = search->bounds;
    return true;
}

bool browser_render__link_bounds(
    const browser_document_t *doc, int link_index, int font_scale, browser_render_link_bounds_t *out_bounds
) {
    browser_render__link_search_t search = {.wanted_index = link_index, .direct = true, .direction = 1};
    return browser_render__run_link_search(doc, font_scale, &search, out_bounds);
}

bool browser_render__adjacent_link(
    const browser_document_t *doc, int link_index, int direction, int font_scale,
    browser_render_link_bounds_t *out_bounds
) {
    if (direction == 0) return false;
    browser_render__link_search_t search = {.reference_index = link_index, .direction = direction};
    return browser_render__run_link_search(doc, font_scale, &search, out_bounds);
}

bool browser_render__link_from_edge(
    const browser_document_t *doc, int edge_y, int direction, int font_scale,
    browser_render_link_bounds_t *out_bounds
) {
    if (direction == 0) return false;
    browser_render__link_search_t search = {.edge_y = edge_y, .direction = direction, .from_edge = true};
    return browser_render__run_link_search(doc, font_scale, &search, out_bounds);
}

typedef struct {
    int after_y;
    int direction;
    bool done;
    bool found;
    browser_render_row_t row;
} browser_render__row_search_t;

static void browser_render__add_row_link(browser_render_row_t *row, int link_index) {
    if (link_index < 0) return;
    /* A multi-word link produces one token per word, all sharing the same
     * link_index -- only record it once per row. */
    if (row->link_count > 0 && row->link_indices[row->link_count - 1] == link_index) return;
    if (row->link_count >= BROWSER_ROW_MAX_LINKS) return;
    row->link_indices[row->link_count++] = link_index;
}

static void browser_render__row_visitor(const browser_layout_token_t *token, void *context) {
    browser_render__row_search_t *search = context;
    if (search->done) return;

    if (search->direction > 0) {
        if (token->y <= search->after_y) return;
        if (!search->found) {
            /* browser_layout__walk() visits tokens in non-decreasing y order,
             * so the first token clearing after_y already has the smallest
             * qualifying y -- it settles which row this is. */
            search->row = (browser_render_row_t){
                .y = token->y, .line_height = token->line_height, .link_count = 0, .image_index = -1
            };
            search->found = true;
        }
        if (token->y != search->row.y) {
            search->done = true; /* Moved past the target row; nothing further matters. */
            return;
        }
    } else {
        if (search->after_y >= 0 && token->y >= search->after_y) {
            search->done = true;
            return;
        }
        if (!search->found || token->y != search->row.y) {
            /* A later (walk order), still-qualifying row supersedes whatever
             * was the best candidate so far -- keep the last one standing. */
            search->row = (browser_render_row_t){
                .y = token->y, .line_height = token->line_height, .link_count = 0, .image_index = -1
            };
            search->found = true;
        }
    }

    if (token->line_height > search->row.line_height) search->row.line_height = token->line_height;
    browser_render__add_row_link(&search->row, token->link_index);
    if (token->image_index >= 0) search->row.image_index = token->image_index;
}

bool browser_render__find_row(
    const browser_document_t *doc, int after_y, int direction, int font_scale, browser_render_row_t *out_row
) {
    if (out_row == NULL || direction == 0) return false;
    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    browser_render__row_search_t search = {.after_y = after_y, .direction = direction, .done = false, .found = false};
    search.row.link_count = 0;
    browser_layout__walk(
        doc, browser_render__content_width(), char_width, char_height, font_scale, browser_render__row_visitor,
        &search
    );
    if (!search.found) return false;
    *out_row = search.row;
    return true;
}

typedef struct {
    const browser_document_t *doc;
    const browser_view_state_t *view;
    const browser_render__theme_t *theme;
    browser_image_cache_t *image_cache;
    int y_top;
    int view_height;
} browser_render__draw_context_t;

static void browser_render__draw_placeholder(
    int x, int y, int w, int h, const char *text, bool selected, const browser_render__theme_t *theme
) {
    (void)display__fill_rect(x, y, w, h, theme->surface);
    (void)display__draw_rect(x, y, w, h, selected ? theme->primary : theme->border);
    (void)display__set_text_color(theme->text_muted);
    (void)display__set_text_size(1);

    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    int text_w = (int)strlen(text) * char_width;
    int text_x = x + (w - text_w) / 2;
    if (text_x < x + 2) text_x = x + 2; /* Longer than the box: pin to the left edge, not off it. */
    (void)display__draw_string(text, text_x, y + h / 2 - char_height / 2);
}

static void browser_render__draw_image_token(
    const browser_render__draw_context_t *ctx, const browser_layout_token_t *token, int screen_y
) {
    const browser_image_ref_t *image = &ctx->doc->images[token->image_index];
    int box_x = BROWSER_CONTENT_MARGIN, box_w = browser_render__content_width();
    bool selected = token->image_index == ctx->view->selected_image;

    /* Never fetches: images load only on an explicit Select press (see
     * browser_app.c's browser_app__load_image()), so a page with several of
     * them scrolls at the same speed as one with none. */
    const image_bitmap_t *bitmap = NULL;
    bruce_result_t result = browser_image_cache__peek(ctx->image_cache, image->url, &bitmap);
    if (result == BRUCE_OK) {
        int draw_x = box_x + (box_w - bitmap->width) / 2;
        int draw_y = screen_y + (token->line_height - bitmap->height) / 2;
        result = display__draw_rgb_bitmap(
            (int16_t)draw_x, (int16_t)draw_y, bitmap->pixels, (int16_t)bitmap->width, (int16_t)bitmap->height
        );
        if (result == BRUCE_OK) {
            if (selected) {
                (void)display__draw_rect(box_x, screen_y, box_w, token->line_height, ctx->theme->primary);
            }
            return;
        }
    }

    const char *alt = image->alt[0] != '\0' ? image->alt : "[image]";
    char text[BROWSER_ALT_MAX + 24];
    if (result == BRUCE_ERR_NOT_FOUND) {
        /* Not yet requested at all -- say so, or there's no way to tell it
         * apart from one that was tried and failed. */
        snprintf(text, sizeof(text), "%s - Select to load", alt);
    } else {
        snprintf(text, sizeof(text), "%s", alt);
    }
    browser_render__draw_placeholder(box_x, screen_y, box_w, token->line_height, text, selected, ctx->theme);
}

static void browser_render__draw_text_token(
    const browser_render__draw_context_t *ctx, const browser_layout_token_t *token, int screen_y
) {
    char word[BROWSER_WORD_BUF_MAX];
    size_t len = token->text_len < sizeof(word) - 1u ? token->text_len : sizeof(word) - 1u;
    memcpy(word, token->text, len);
    word[len] = '\0';

    int scale = browser_layout__heading_scale(token->heading_level, ctx->view->font_scale);
    bool selected = token->link_index >= 0 && token->link_index == ctx->view->selected_link;
    bruce_display_color_t fg = ctx->theme->text;
    if (token->heading_level > 0) fg = ctx->theme->secondary;
    else if (token->link_index >= 0) fg = ctx->theme->primary;
    if (selected) fg = ctx->theme->background;

    int x = BROWSER_CONTENT_MARGIN + token->x;
    if (selected) {
        int16_t char_width, char_height;
        browser_render__metrics(&char_width, &char_height);
        (void)display__fill_rect(
            (int16_t)x, (int16_t)screen_y, (int16_t)((int)len * char_width * scale), (int16_t)(char_height * scale),
            ctx->theme->primary
        );
    }
    (void)display__set_text_size((uint8_t)scale);
    (void)display__set_text_color(fg);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__draw_string(word, x, screen_y);
}

static void browser_render__token_visitor(const browser_layout_token_t *token, void *context) {
    browser_render__draw_context_t *ctx = context;
    int screen_y = ctx->y_top + (token->y - ctx->view->scroll_y);
    if (screen_y + token->line_height <= ctx->y_top || screen_y >= ctx->y_top + ctx->view_height) return;

    if (token->image_index >= 0) browser_render__draw_image_token(ctx, token, screen_y);
    else if (token->text_len > 0) browser_render__draw_text_token(ctx, token, screen_y);
}

static void browser_render__draw_chrome(
    const browser_document_t *doc, const browser_history_t *history, int progress,
    const browser_render__theme_t *theme
) {
    int width = display__width();
    int height = browser_render__chrome_height();
    (void)display__fill_rect(0, 0, width, height, theme->surface);
    if (progress >= 0) {
        if (progress > 100) progress = 100;
        int progress_width = width * progress / 100;
        if (progress_width > 0)
            (void)display__fill_rect(0, 0, progress_width, height, theme->primary);
    }

    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    int text_y = BROWSER_CHROME_PADDING;

    (void)display__set_text_size(1);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(
        browser_history__can_go_back(history) ? theme->text : theme->text_muted
    );
    (void)display__draw_string("<", BROWSER_CONTENT_MARGIN, text_y);
    (void)display__set_text_color(
        browser_history__can_go_forward(history) ? theme->text : theme->text_muted
    );
    (void)display__draw_string(">", BROWSER_CONTENT_MARGIN + char_width + 4, text_y);

    int url_x = BROWSER_CONTENT_MARGIN + 2 * (char_width + 4);
    int max_chars = (width - url_x - BROWSER_CONTENT_MARGIN) / char_width;
    if (max_chars < 0) max_chars = 0;
    /* A plain memcpy, not snprintf("%s", ...), because GCC's
     * -Werror=format-truncation can't see that `cap` (runtime-computed from
     * the display width) already bounds doc->url's copy into `shown` --  it
     * only sees doc->url's static array size (BROWSER_URL_MAX) against
     * sizeof(shown) and flags the deliberate truncation as an error. */
    char shown[96];
    size_t cap = (size_t)max_chars < sizeof(shown) - 1u ? (size_t)max_chars : sizeof(shown) - 1u;
    size_t url_len = strlen(doc->url);
    size_t copy_len = url_len < cap ? url_len : cap;
    memcpy(shown, doc->url, copy_len);
    shown[copy_len] = '\0';
    (void)display__set_text_color(theme->text);
    (void)display__draw_string(shown, url_x, text_y);
}

bruce_result_t browser_render__draw(
    const browser_document_t *doc, const browser_view_state_t *view, const browser_history_t *history,
    browser_image_cache_t *image_cache
) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    browser_render__theme_t theme;
    browser_render__get_theme(&theme);
    (void)display__fill_screen(theme.background);

    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    browser_render__draw_context_t ctx = {
        .doc = doc,
        .view = view,
        .theme = &theme,
        .image_cache = image_cache,
        .y_top = browser_render__chrome_height(),
        .view_height = browser_render__view_height(),
    };
    if (ctx.view_height > 0) {
        browser_layout__walk(
            doc, browser_render__content_width(), char_width, char_height, view->font_scale,
            browser_render__token_visitor, &ctx
        );
    }

    /* Drawn last, on top of the content: token_visitor only skips a row once
     * it's entirely outside the viewport, so a row straddling y_top (its
     * scroll position landing mid-row rather than exactly on a row
     * boundary -- e.g. after a page-scroll press) still draws in full, and
     * without this the part of it above y_top would bleed over the chrome
     * bar instead of being covered by it. */
    browser_render__draw_chrome(doc, history, -1, &theme);

    return display__present();
}

bruce_result_t browser_render__draw_loading(
    const browser_document_t *doc, const browser_history_t *history, int progress
) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    browser_render__theme_t theme;
    browser_render__get_theme(&theme);
    (void)display__fill_screen(theme.background);
    browser_render__draw_chrome(doc, history, progress < 0 ? 0 : progress, &theme);
    return display__present();
}
