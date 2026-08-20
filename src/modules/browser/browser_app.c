#include "browser_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "browser_debug.h"
#include "browser_document.h"
#include "browser_history.h"
#include "browser_image_cache.h"
#include "browser_page.h"
#include "browser_render.h"
#include "core_sdk/dialog.h"
#include "core_sdk/input.h"
#include "core_sdk/notification.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"

typedef struct {
    browser_document_t *doc;
    browser_history_t *history;
    browser_image_cache_t *image_cache;
    browser_view_state_t view;
} browser_app_state_t;

typedef struct {
    browser_app_state_t *state;
    int last_percent;
} browser_app_progress_t;

static void browser_app__show_progress(size_t received, void *context) {
    browser_app_progress_t *progress = context;
    /* Content-Length is not exposed by the streaming HTTP callback. Advance
     * quickly for small pages and asymptotically reserve the final segment
     * for request completion instead of displaying a false exact percent. */
    int percent = 8 + (int)(received * 84u / (received + 32768u));
    if (percent > 92) percent = 92;
    if (percent < progress->last_percent + 2) return;
    progress->last_percent = percent;
    (void)browser_render__draw_loading(progress->state->doc, progress->state->history, percent);
}

static size_t browser_app__page_url_len(const char *url) {
    const char *fragment = strchr(url, '#');
    size_t len = fragment != NULL ? (size_t)(fragment - url) : strlen(url);
    while (len > 0 && url[len - 1] == '/') len--;
    return len;
}

static bool browser_app__scroll_to_fragment(browser_app_state_t *state, const char *url) {
    const char *fragment = strchr(url, '#');
    if (fragment == NULL) return false;
    size_t current_len = browser_app__page_url_len(state->doc->url);
    size_t target_len = browser_app__page_url_len(url);
    if (current_len != target_len || memcmp(state->doc->url, url, current_len) != 0) return false;

    size_t item_index = 0;
    int scroll_y = 0;
    if (fragment[1] != '\0') {
        if (!browser_document__find_anchor(state->doc, fragment + 1, &item_index)) return true;
        scroll_y = browser_render__item_y(state->doc, item_index, state->view.font_scale);
    }
    browser_document__set_url(state->doc, url);
    int max_scroll = browser_render__max_scroll(state->doc, state->view.font_scale);
    state->view.scroll_y = scroll_y < max_scroll ? scroll_y : max_scroll;
    state->view.selected_link = -1;
    state->view.selected_image = -1;
    state->view.row_y = -1;
    return true;
}

/* Mirrors modules/filemanager's own pattern: input__read() surfaces
 * BRUCE_ERR_NOT_FOREGROUND when another process takes over the screen (e.g. a
 * permission prompt); wait here until we're either back in the foreground or
 * the process is gone for good. */
static bool browser_app__resume_after_handoff(void) {
    bruce_process_snapshot_t snapshot;
    bruce_process_id_t self = process__current_id();
    if (self == BRUCE_PROCESS_ID_INVALID || process__snapshot(self, &snapshot) != BRUCE_OK ||
        snapshot.state != BRUCE_PROCESS_BACKGROUND) {
        return false;
    }
    do {
        if (runtime__delay(20) != BRUCE_OK || process__snapshot(self, &snapshot) != BRUCE_OK) return false;
    } while (snapshot.state == BRUCE_PROCESS_BACKGROUND);
    return snapshot.state == BRUCE_PROCESS_FOREGROUND;
}

static void browser_app__navigate(browser_app_state_t *state, const char *raw_url, bool push_history) {
    char url[BROWSER_URL_MAX];
    if (!browser_page__normalize_url(raw_url, url, sizeof(url))) {
        (void)notification__push("Invalid URL", 2000);
        return;
    }

    browser_document__reset(state->doc);
    browser_document__set_url(state->doc, url);
    (void)browser_render__draw_loading(state->doc, state->history, 0);
    int status_code = 0;
    browser_app_progress_t progress = {.state = state, .last_percent = 0};
    bruce_result_t result = browser_page__fetch(
        url, state->doc, &status_code, browser_app__show_progress, &progress
    );

    if (result != BRUCE_OK) {
        browser_document__reset(state->doc);
        browser_document__set_url(state->doc, url);
        browser_document__set_title(state->doc, "Error", 5);
        char message[96];
        snprintf(message, sizeof(message), "Could not load this page: %s", result__to_string(result));
        browser_document__add_text(state->doc, message, strlen(message), 0, -1);
    } else if (status_code < 200 || status_code >= 300) {
        char note[32];
        snprintf(note, sizeof(note), "HTTP %d", status_code);
        (void)notification__push(note, 2500);
    }

    if (push_history) browser_history__push(state->history, url);
    state->view.scroll_y = 0;
    state->view.selected_link = -1;
    state->view.selected_image = -1;
    state->view.row_y = -1;
}

/* Fetches the currently selected image over HTTP, if it hasn't been already.
 * See browser_image_cache.h's file comment for why this is the only place
 * that ever calls browser_image_cache__get() -- drawing only ever peek()s,
 * so a page with several images scrolls exactly as fast as one with none;
 * loading one is a deliberate, explicit action. */
static void browser_app__load_image(browser_app_state_t *state) {
    if (state->view.selected_image < 0) return;
    const char *url = state->doc->images[state->view.selected_image].url;
    const void *data = NULL;
    size_t len = 0;
    if (browser_image_cache__peek(state->image_cache, url, &data, &len) == BRUCE_ERR_NOT_FOUND) {
        (void)notification__push("Loading image...", 30000);
        (void)browser_image_cache__get(state->image_cache, url, &data, &len);
        (void)notification__dismiss();
    }
}

/* Adjusts scroll_y by the minimum amount needed to bring [top, top +
 * line_height) into the visible viewport -- the whole row, not just its top
 * edge, or a row taller than the base line height (e.g. a heading) could end
 * up with only its first pixel row on screen and the rest scrolled off the
 * bottom. */
static void browser_app__scroll_into_view(browser_app_state_t *state, int top, int line_height) {
    int bottom = top + line_height;
    int view_height = browser_render__view_height();
    if (top < state->view.scroll_y) state->view.scroll_y = top;
    else if (bottom > state->view.scroll_y + view_height) {
        state->view.scroll_y = bottom - view_height;
        /* The row itself is taller than the whole viewport: fall back to
         * showing its top edge rather than its (unreachable) bottom. */
        if (state->view.scroll_y > top) state->view.scroll_y = top;
    }

    int max_scroll = browser_render__max_scroll(state->doc, state->view.font_scale);
    if (state->view.scroll_y > max_scroll) state->view.scroll_y = max_scroll;
    if (state->view.scroll_y < 0) state->view.scroll_y = 0;
}

/* Up/Down moves one content row (word-wrapped line) at a time, not link to
 * link -- the screen only scrolls once the cursor row would go off it, same
 * as any ordinary list/pager. A row's link(s) or image (a row is one or the
 * other, never both -- see browser_render_row_t) are highlighted as the
 * cursor reaches them; a row carrying more than one distinct link is cycled
 * through link by link, in the same direction, before the cursor moves on to
 * the next row. Select then either navigates the highlighted link or loads
 * the highlighted image -- see browser_app__load_image(). */
static void browser_app__move_line(browser_app_state_t *state, int direction) {
    if (state->doc->item_count == 0) return;

    /* Fragment jumps and page scrolling move the viewport independently of
     * the row cursor. If that cursor is now off-screen, start navigation at
     * the visible edge instead of dragging the viewport back to its stale
     * document position. find_row() uses strict comparisons, hence top - 1
     * for Down and bottom for Up. */
    int view_top = state->view.scroll_y;
    int view_bottom = view_top + browser_render__view_height();
    if (state->view.row_y < view_top || state->view.row_y >= view_bottom) {
        state->view.row_y = direction > 0 ? view_top - 1 : view_bottom;
        state->view.selected_link = -1;
        state->view.selected_image = -1;
    }

    if (state->view.row_y >= 0) {
        /* Re-fetch the current row's own link list: find_row(row_y - 1, +1)
         * is exactly "the row at row_y" (there's no room for another row's y
         * to fall strictly between row_y - 1 and row_y). */
        browser_render_row_t row;
        if (browser_render__find_row(state->doc, state->view.row_y - 1, 1, state->view.font_scale, &row) &&
            row.y == state->view.row_y) {
            int pos = -1;
            for (int i = 0; i < row.link_count; ++i) {
                if (row.link_indices[i] == state->view.selected_link) {
                    pos = i;
                    break;
                }
            }
            int next_pos = pos + direction;
            if (pos >= 0 && next_pos >= 0 && next_pos < row.link_count) {
                state->view.selected_link = row.link_indices[next_pos];
                browser_app__scroll_into_view(state, row.y, row.line_height);
                return;
            }
        }
    }

    browser_render_row_t row;
    if (!browser_render__find_row(state->doc, state->view.row_y, direction, state->view.font_scale, &row)) return;
    state->view.row_y = row.y;
    if (row.link_count > 0) {
        state->view.selected_link = row.link_indices[direction > 0 ? 0 : row.link_count - 1];
        state->view.selected_image = -1;
    } else {
        state->view.selected_link = -1;
        state->view.selected_image = row.image_index; /* -1 for a plain-text row. */
    }
    browser_app__scroll_into_view(state, row.y, row.line_height);
}

static void browser_app__page_scroll(browser_app_state_t *state, int direction) {
    int view_height = browser_render__view_height();
    if (view_height <= 0) view_height = 1;
    int max_scroll = browser_render__max_scroll(state->doc, state->view.font_scale);
    state->view.scroll_y += direction * view_height;
    if (state->view.scroll_y < 0) state->view.scroll_y = 0;
    if (state->view.scroll_y > max_scroll) state->view.scroll_y = max_scroll;
}

/* +/- resizes body/heading text on the fly (see BROWSER_FONT_SCALE_MIN/MAX).
 * Re-flowing at a new scale moves every row's y, so an exact scroll_y or
 * row_y/selected_link carried over from the old layout would land on the
 * wrong content -- keep the same approximate reading position by scaling
 * scroll_y by how much the total content height changed, and drop row
 * selection rather than have it silently point at the wrong row. */
static void browser_app__adjust_font_scale(browser_app_state_t *state, int delta) {
    int old_scale = state->view.font_scale;
    int new_scale = old_scale + delta;
    if (new_scale < BROWSER_FONT_SCALE_MIN) new_scale = BROWSER_FONT_SCALE_MIN;
    if (new_scale > BROWSER_FONT_SCALE_MAX) new_scale = BROWSER_FONT_SCALE_MAX;
    if (new_scale == old_scale) return;

    int old_height = browser_render__content_height(state->doc, old_scale);
    int new_height = browser_render__content_height(state->doc, new_scale);
    state->view.font_scale = new_scale;
    state->view.scroll_y = old_height > 0 ? (int)((int64_t)state->view.scroll_y * new_height / old_height) : 0;

    int max_scroll = browser_render__max_scroll(state->doc, new_scale);
    if (state->view.scroll_y > max_scroll) state->view.scroll_y = max_scroll;
    if (state->view.scroll_y < 0) state->view.scroll_y = 0;
    state->view.selected_link = -1;
    state->view.selected_image = -1;
    state->view.row_y = -1;
}

static void browser_app__edit_url(browser_app_state_t *state) {
    char buffer[BROWSER_URL_MAX];
    snprintf(buffer, sizeof(buffer), "%s", state->doc->url);
    if (dialog__text_input("Go to URL", NULL, buffer, false, buffer, sizeof(buffer)) != BRUCE_OK) return;
    if (buffer[0] == '\0') return;
    browser_app__navigate(state, buffer, true);
}

static bool browser_app__handle_event(browser_app_state_t *state, const bruce_input_event_t *event) {
    if (event->action != BRUCE_INPUT_PRESS) return true;

    if (event->code == BRUCE_INPUT_CODE_LEFT || event->code == BRUCE_INPUT_CODE_BACK) {
        const char *url = browser_history__back(state->history);
        if (url == NULL) return false; /* No history left: exit the app, like Back everywhere else. */
        browser_app__navigate(state, url, false);
    } else if (event->code == BRUCE_INPUT_CODE_RIGHT || event->code == BRUCE_INPUT_CODE_SELECT) {
        if (state->view.selected_link >= 0) {
            const char *url = state->doc->links[state->view.selected_link].url;
            if (!browser_app__scroll_to_fragment(state, url)) browser_app__navigate(state, url, true);
        } else if (state->view.selected_image >= 0) {
            browser_app__load_image(state);
        }
    } else if (event->code == BRUCE_INPUT_CODE_UP) {
        browser_app__move_line(state, -1);
    } else if (event->code == BRUCE_INPUT_CODE_DOWN) {
        browser_app__move_line(state, 1);
    } else if (event->code == ' ' || event->code == BRUCE_INPUT_CODE_NEXT) {
        browser_app__page_scroll(state, 1);
    } else if (event->code == 'b' || event->code == BRUCE_INPUT_CODE_PREV) {
        browser_app__page_scroll(state, -1);
    } else if (event->code == '-') {
        browser_app__adjust_font_scale(state, -1);
    } else if (event->code == '=') {
        browser_app__adjust_font_scale(state, 1);
    } else if (event->code == 'p') {
        browser_debug__dump(state->doc, state->view.font_scale);
    } else if (event->code == 'g') {
        browser_app__edit_url(state);
    } else if (event->code == 'r') {
        browser_app__navigate(state, state->doc->url, false);
    } else if (event->code == BRUCE_INPUT_CODE_HOME) {
        browser_app__navigate(state, BROWSER_HOME_URL, true);
    }
    return true;
}

int browser_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Browse the web, Lynx-style, with inline images.");
    ap_add_optional_arg(parser, "url", "Starting URL (defaults to " BROWSER_HOME_URL ")");
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        return status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
    }
    char start_url[BROWSER_URL_MAX];
    const char *arg_url = ap_get_arg(parser, "url");
    snprintf(start_url, sizeof(start_url), "%s", arg_url != NULL ? arg_url : BROWSER_HOME_URL);
    ap_free(parser);

    browser_app_state_t state = {
        .view = {.scroll_y = 0, .selected_link = -1, .selected_image = -1, .row_y = -1, .font_scale = 0}
    };
    bruce_result_t result = browser_document__create(&state.doc);
    if (result == BRUCE_OK) result = browser_history__create(&state.history);
    if (result == BRUCE_OK) result = browser_image_cache__create(&state.image_cache);
    if (result != BRUCE_OK) {
        browser_document__destroy(state.doc);
        browser_history__destroy(state.history);
        browser_image_cache__destroy(state.image_cache);
        return result;
    }

    browser_app__navigate(&state, start_url, true);
    (void)input__flush();
    (void)browser_render__draw(state.doc, &state.view, state.history, state.image_cache);

    for (;;) {
        bruce_input_event_t event;
        bruce_result_t read_result = input__read(&event, UINT32_MAX);
        if (read_result == BRUCE_ERR_NOT_FOREGROUND) {
            if (browser_app__resume_after_handoff()) {
                (void)input__flush();
                (void)browser_render__draw(state.doc, &state.view, state.history, state.image_cache);
                continue;
            }
            break;
        }
        if (read_result != BRUCE_OK) continue;
        if (!browser_app__handle_event(&state, &event)) break;
        (void)browser_render__draw(state.doc, &state.view, state.history, state.image_cache);
    }

    browser_document__destroy(state.doc);
    browser_history__destroy(state.history);
    browser_image_cache__destroy(state.image_cache);
    return 0;
}
