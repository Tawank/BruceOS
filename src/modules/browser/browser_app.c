#include "browser_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
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

    (void)notification__push("Loading...", 30000);
    int status_code = 0;
    bruce_result_t result = browser_page__fetch(url, state->doc, &status_code);
    (void)notification__dismiss();

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
}

/* Adjusts scroll_y by the minimum amount needed to bring the selected link's
 * top edge into the visible viewport. */
static void browser_app__scroll_into_view(browser_app_state_t *state) {
    if (state->view.selected_link < 0) return;
    int top = browser_render__link_top(state->doc, state->view.selected_link);
    if (top < 0) return;
    int view_height = browser_render__view_height();
    if (top < state->view.scroll_y) state->view.scroll_y = top;
    else if (top >= state->view.scroll_y + view_height) state->view.scroll_y = top - view_height + 1;

    int max_scroll = browser_render__max_scroll(state->doc);
    if (state->view.scroll_y > max_scroll) state->view.scroll_y = max_scroll;
    if (state->view.scroll_y < 0) state->view.scroll_y = 0;
}

static void browser_app__move_link_selection(browser_app_state_t *state, int delta) {
    size_t link_count = state->doc->link_count;
    if (link_count == 0) return;
    int next = state->view.selected_link;
    if (next < 0) next = delta > 0 ? 0 : (int)link_count - 1;
    else {
        next += delta;
        if (next < 0) next = 0;
        if (next >= (int)link_count) next = (int)link_count - 1;
    }
    state->view.selected_link = next;
    browser_app__scroll_into_view(state);
}

static void browser_app__page_scroll(browser_app_state_t *state, int direction) {
    int view_height = browser_render__view_height();
    if (view_height <= 0) view_height = 1;
    int max_scroll = browser_render__max_scroll(state->doc);
    state->view.scroll_y += direction * view_height;
    if (state->view.scroll_y < 0) state->view.scroll_y = 0;
    if (state->view.scroll_y > max_scroll) state->view.scroll_y = max_scroll;
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
            browser_app__navigate(state, url, true);
        }
    } else if (event->code == BRUCE_INPUT_CODE_UP) {
        browser_app__move_link_selection(state, -1);
    } else if (event->code == BRUCE_INPUT_CODE_DOWN) {
        browser_app__move_link_selection(state, 1);
    } else if (event->code == ' ' || event->code == BRUCE_INPUT_CODE_NEXT) {
        browser_app__page_scroll(state, 1);
    } else if (event->code == 'b' || event->code == BRUCE_INPUT_CODE_PREV) {
        browser_app__page_scroll(state, -1);
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

    browser_app_state_t state = {.view = {.scroll_y = 0, .selected_link = -1}};
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
