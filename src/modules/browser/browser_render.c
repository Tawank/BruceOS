#include "browser_render.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "browser_image_draw.h"
#include "browser_layout.h"
#include "core_sdk/display.h"

#define BROWSER_CONTENT_MARGIN 2
#define BROWSER_CHROME_PADDING 5
#define BROWSER_WORD_BUF_MAX 64

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

int browser_render__content_height(const browser_document_t *doc) {
    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    return browser_layout__walk(
        doc, browser_render__content_width(), char_width, char_height, browser_render__noop_visitor, NULL
    );
}

int browser_render__max_scroll(const browser_document_t *doc) {
    int max_scroll = browser_render__content_height(doc) - browser_render__view_height();
    return max_scroll > 0 ? max_scroll : 0;
}

typedef struct {
    int link_index;
    int found_y;
} browser_render__link_search_t;

static void browser_render__link_top_visitor(const browser_layout_token_t *token, void *context) {
    browser_render__link_search_t *search = context;
    if (search->found_y >= 0 || token->link_index != search->link_index) return;
    search->found_y = token->y;
}

int browser_render__link_top(const browser_document_t *doc, int link_index) {
    if (link_index < 0) return -1;
    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    browser_render__link_search_t search = {.link_index = link_index, .found_y = -1};
    browser_layout__walk(
        doc, browser_render__content_width(), char_width, char_height, browser_render__link_top_visitor, &search
    );
    return search.found_y;
}

typedef struct {
    const browser_document_t *doc;
    const browser_view_state_t *view;
    browser_image_cache_t *image_cache;
    int y_top;
    int view_height;
} browser_render__draw_context_t;

static void browser_render__draw_placeholder(int x, int y, int w, int h, const char *alt) {
    (void)display__draw_rect(x, y, w, h, BRUCE_COLOR_DARKGREY);
    (void)display__set_text_color(BRUCE_COLOR_LIGHTGREY);
    (void)display__set_text_size(1);
    (void)display__draw_string(alt != NULL && alt[0] != '\0' ? alt : "[image]", x + 4, y + h / 2 - 4);
}

static void browser_render__draw_image_token(
    const browser_render__draw_context_t *ctx, const browser_layout_token_t *token, int screen_y
) {
    const browser_image_ref_t *image = &ctx->doc->images[token->image_index];
    int box_x = BROWSER_CONTENT_MARGIN, box_w = browser_render__content_width();
    const void *data = NULL;
    size_t len = 0;
    bruce_result_t result = browser_image_cache__get(ctx->image_cache, image->url, &data, &len);
    if (result == BRUCE_OK) {
        result = browser_image_draw__fit(
            data, len, box_x, screen_y, box_w, token->line_height, BRUCE_COLOR_BLACK, NULL, NULL
        );
    }
    if (result != BRUCE_OK) browser_render__draw_placeholder(box_x, screen_y, box_w, token->line_height, image->alt);
}

static void browser_render__draw_text_token(
    const browser_render__draw_context_t *ctx, const browser_layout_token_t *token, int screen_y
) {
    char word[BROWSER_WORD_BUF_MAX];
    size_t len = token->text_len < sizeof(word) - 1u ? token->text_len : sizeof(word) - 1u;
    memcpy(word, token->text, len);
    word[len] = '\0';

    int scale = browser_layout__heading_scale(token->heading_level);
    bool selected = token->link_index >= 0 && token->link_index == ctx->view->selected_link;
    bruce_display_color_t fg = BRUCE_COLOR_WHITE;
    if (token->heading_level > 0) fg = BRUCE_COLOR_YELLOW;
    else if (token->link_index >= 0) fg = BRUCE_COLOR_CYAN;
    if (selected) fg = BRUCE_COLOR_BLACK;

    int x = BROWSER_CONTENT_MARGIN + token->x;
    if (selected) {
        int16_t char_width, char_height;
        browser_render__metrics(&char_width, &char_height);
        (void)display__fill_rect(
            (int16_t)x, (int16_t)screen_y, (int16_t)((int)len * char_width * scale), (int16_t)(char_height * scale),
            BRUCE_COLOR_CYAN
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

static void browser_render__draw_chrome(const browser_document_t *doc, const browser_history_t *history) {
    int width = display__width();
    int height = browser_render__chrome_height();
    (void)display__fill_rect(0, 0, width, height, BRUCE_COLOR_NAVY);

    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    int text_y = BROWSER_CHROME_PADDING;

    (void)display__set_text_size(1);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_color(
        browser_history__can_go_back(history) ? BRUCE_COLOR_WHITE : BRUCE_COLOR_DARKGREY
    );
    (void)display__draw_string("<", BROWSER_CONTENT_MARGIN, text_y);
    (void)display__set_text_color(
        browser_history__can_go_forward(history) ? BRUCE_COLOR_WHITE : BRUCE_COLOR_DARKGREY
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
    (void)display__set_text_color(BRUCE_COLOR_WHITE);
    (void)display__draw_string(shown, url_x, text_y);
}

bruce_result_t browser_render__draw(
    const browser_document_t *doc, const browser_view_state_t *view, const browser_history_t *history,
    browser_image_cache_t *image_cache
) {
    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    (void)display__fill_screen(BRUCE_COLOR_BLACK);

    browser_render__draw_chrome(doc, history);

    int16_t char_width, char_height;
    browser_render__metrics(&char_width, &char_height);
    browser_render__draw_context_t ctx = {
        .doc = doc,
        .view = view,
        .image_cache = image_cache,
        .y_top = browser_render__chrome_height(),
        .view_height = browser_render__view_height(),
    };
    if (ctx.view_height > 0) {
        browser_layout__walk(
            doc, browser_render__content_width(), char_width, char_height, browser_render__token_visitor, &ctx
        );
    }

    return display__present();
}
