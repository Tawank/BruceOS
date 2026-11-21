#include <stdio.h>
#include <string.h>

#include "bruce_sdk.h"
#include "store.h"
#include "ui.h"

#define MESSAGES_IDLE_DELAY_MS 30
#define MESSAGES_POLL_MS 1000
#define MESSAGES_LONG_PRESS_MS 500
#define MESSAGES_SYNC_BATCH 4

typedef struct {
    ui_t ui;
    store_t *store;
    bool running;
    bool dirty;
    uint64_t polled_at;
    uint32_t message_sequence;       /* Of the last sync with the SIM. */
    bruce_dialog_window_area_t area; /* Inside the launcher's window frame. */
    uint64_t drawn_at;
} messages_t;

static int64_t messages__now_utc(void) {
    bruce_clock_datetime_t now;
    int64_t epoch = 0;
    if (clock__get_utc(&now) != BRUCE_OK || clock__datetime_to_epoch(&now, &epoch) != BRUCE_OK) return 0;
    return epoch;
}

static void messages__refresh_threads(messages_t *app) {
    app->ui.thread_count = store__threads(app->store, app->ui.threads, STORE_MESSAGES_MAX);
    int rows = (int)app->ui.thread_count + 1;
    if (app->ui.selected >= rows) app->ui.selected = rows - 1;
    app->dirty = true;
}

static void messages__save(messages_t *app) {
    if (!store__save(app->store)) dialog__message(BRUCE_DIALOG_ERROR, "Messages", "Could not save messages");
}

/* Moves received messages from the SIM into the store: saved first, then deleted. */
static void messages__sync(messages_t *app) {
    static bruce_cellular_message_t received[MESSAGES_SYNC_BATCH];
    bool added = false;
    for (int round = 0; round < 16; ++round) {
        size_t count = 0;
        if (cellular__get_messages(received, MESSAGES_SYNC_BATCH, &count) != BRUCE_OK || count == 0) break;
        for (size_t i = 0; i < count; ++i) {
            int64_t utc = received[i].received_utc > 0 ? received[i].received_utc : messages__now_utc();
            store__add(app->store, utc, true, true, received[i].number, received[i].text);
        }
        if (!store__save(app->store)) break;
        added = true;
        bool deleted = true;
        for (size_t i = 0; i < count; ++i)
            deleted = cellular__delete_message(received[i].id) == BRUCE_OK && deleted;
        if (!deleted) break;
    }
    if (!added) return;
    if (app->ui.screen == UI_SCREEN_THREAD && store__mark_read(app->store, app->ui.number))
        messages__save(app);
    messages__refresh_threads(app);
}

static void messages__poll(messages_t *app, bool now) {
    uint64_t time = runtime__now();
    if (!now && time - app->polled_at < MESSAGES_POLL_MS) return;
    app->polled_at = time;
    bruce_cellular_status_t status;
    if (cellular__get_status(&status) != BRUCE_OK) return;
    if (status.state != app->ui.status.state) app->dirty = true;
    app->ui.status = status;
    if (status.message_sequence != app->message_sequence && status.state == BRUCE_CELLULAR_READY) {
        app->message_sequence = status.message_sequence;
        messages__sync(app);
    }
    int64_t utc = messages__now_utc();
    if (utc / 60 != app->ui.now_utc / 60) {
        app->ui.now_utc = utc;
        app->ui.utc_offset = clock__get_local_offset_seconds();
        app->dirty = true;
    }
}

static void messages__open(messages_t *app, const char *number) {
    snprintf(app->ui.number, sizeof(app->ui.number), "%s", number);
    app->ui.screen = UI_SCREEN_THREAD;
    app->ui.thread_scroll = 0;
    app->ui.draft[0] = '\0';
    if (store__mark_read(app->store, number)) {
        messages__save(app);
        messages__refresh_threads(app);
    }
    app->dirty = true;
}

static void messages__close(messages_t *app) {
    app->ui.screen = UI_SCREEN_LIST;
    app->ui.draft[0] = '\0';
    messages__refresh_threads(app);
}

static bool messages__valid_number(const char *number) {
    if (number[0] == '\0') return false;
    for (const char *c = number; *c != '\0'; ++c) {
        if (!((*c >= '0' && *c <= '9') || (*c == '+' && c == number))) return false;
    }
    return true;
}

/* Pick a contact or type a number. */
static void messages__new(messages_t *app) {
    const store_t *store = app->store;
    size_t count = store->contact_count + 1;
    bruce_dialog_choice_t *choices = memory__calloc(count, sizeof(*choices));
    if (choices == NULL) return;
    choices[0] = (bruce_dialog_choice_t){.label = "Type a number", .icon_name = "dialpad"};
    for (size_t i = 0; i < store->contact_count; ++i) {
        choices[i + 1] = (bruce_dialog_choice_t){
            .label = store->contacts[i].name, .right_text = store->contacts[i].number, .icon_name = "account"
        };
    }
    size_t selected = 0;
    bool chosen = dialog__choice("New message", NULL, choices, count, &selected) == BRUCE_OK;
    memory__free(choices);
    app->dirty = true;
    if (!chosen) return;
    if (selected > 0) {
        messages__open(app, store->contacts[selected - 1].number);
        return;
    }
    char number[BRUCE_CELLULAR_NUMBER_MAX] = "";
    if (dialog__text_input("New message", "Number", NULL, false, number, sizeof(number)) != BRUCE_OK) return;
    if (!messages__valid_number(number)) {
        dialog__message(BRUCE_DIALOG_ERROR, "New message", "Use digits, with + only first");
        return;
    }
    messages__open(app, number);
}

static bool messages__draw(messages_t *app);

static void messages__send(messages_t *app) {
    if (app->ui.draft[0] == '\0') return;
    if (!ui__draft_fits(app->ui.draft)) {
        dialog__message(BRUCE_DIALOG_ERROR, "Messages", "Too long for one message");
        app->dirty = true;
        return;
    }
    app->ui.sending = true;
    messages__draw(app);
    bruce_result_t result = cellular__send_message(app->ui.number, app->ui.draft);
    app->ui.sending = false;
    if (result == BRUCE_OK) {
        store__add(app->store, messages__now_utc(), false, false, app->ui.number, app->ui.draft);
        messages__save(app);
        app->ui.draft[0] = '\0';
        app->ui.thread_scroll = 0;
        messages__refresh_threads(app);
    } else {
        const char *text = result == BRUCE_ERR_INVALID_STATE && app->ui.status.state != BRUCE_CELLULAR_READY
                               ? ui__state_text(app->ui.status.state)
                               : "Not sent";
        dialog__message(BRUCE_DIALOG_ERROR, "Messages", text);
    }
    app->dirty = true;
}

/* The on-screen keyboard; it types ASCII. */
static void messages__compose(messages_t *app) {
    char text[sizeof(app->ui.draft)];
    if (dialog__text_input("Message", NULL, app->ui.draft, false, text, sizeof(text)) == BRUCE_OK)
        snprintf(app->ui.draft, sizeof(app->ui.draft), "%s", text);
    app->dirty = true;
}

static void messages__type(messages_t *app, char c) {
    size_t length = strlen(app->ui.draft);
    if (length + 1 < sizeof(app->ui.draft)) {
        app->ui.draft[length] = c;
        app->ui.draft[length + 1] = '\0';
    }
    app->dirty = true;
}

static void messages__backspace(messages_t *app) {
    size_t length = strlen(app->ui.draft);
    while (length > 0 && ((unsigned char)app->ui.draft[length - 1] & 0xC0) == 0x80) length--;
    if (length > 0) length--;
    app->ui.draft[length] = '\0';
    app->dirty = true;
}

static bool messages__confirm(const char *title, const char *message, const char *action) {
    bruce_dialog_choice_t choices[] = {{.label = "Cancel"}, {.label = action}};
    size_t selected = 0;
    return dialog__choice(title, message, choices, 2, &selected) == BRUCE_OK && selected == 1;
}

/* The conversation on the selected row, or the open one. */
static const char *messages__current_number(const messages_t *app) {
    if (app->ui.screen == UI_SCREEN_THREAD) return app->ui.number;
    int row = app->ui.selected;
    if (row < 1 || row > (int)app->ui.thread_count) return NULL;
    return app->store->messages[app->ui.threads[row - 1].last].number;
}

static void messages__menu(messages_t *app) {
    const char *number = messages__current_number(app);
    bruce_dialog_choice_t choices[4];
    int actions[4];
    size_t count = 0;
    enum { MENU_DELETE, MENU_EXIT, MENU_BACK };
    if (number != NULL) {
        choices[count] = (bruce_dialog_choice_t){.label = "Delete conversation"};
        actions[count++] = MENU_DELETE;
    }
    choices[count] = (bruce_dialog_choice_t){.label = "Exit"};
    actions[count++] = MENU_EXIT;
    choices[count] = (bruce_dialog_choice_t){.label = "Back"};
    actions[count++] = MENU_BACK;
    size_t selected = 0;
    app->dirty = true;
    if (dialog__choice("Messages", NULL, choices, count, &selected) != BRUCE_OK) return;
    if (actions[selected] == MENU_DELETE) {
        char copy[BRUCE_CELLULAR_NUMBER_MAX];
        snprintf(copy, sizeof(copy), "%s", number);
        if (!messages__confirm("Delete conversation", copy, "Delete")) return;
        store__delete_thread(app->store, copy);
        messages__save(app);
        if (app->ui.screen == UI_SCREEN_THREAD) messages__close(app);
        messages__refresh_threads(app);
    } else if (actions[selected] == MENU_EXIT) {
        app->running = false;
    }
}

static void messages__open_row(messages_t *app, int row) {
    app->ui.selected = row;
    if (row == 0) messages__new(app);
    else if (row <= (int)app->ui.thread_count)
        messages__open(app, app->store->messages[app->ui.threads[row - 1].last].number);
    app->dirty = true;
}

static void messages__scroll(messages_t *app, int delta, int width, int height) {
    int scroll = app->ui.thread_scroll + delta;
    int max = ui__thread_max_scroll(&app->ui, width, height);
    app->ui.thread_scroll = scroll < 0 ? 0 : scroll > max ? max : scroll;
    app->dirty = true;
}

static void messages__key(messages_t *app, int32_t code, int width, int height) {
    if (code == BRUCE_INPUT_CODE_MENU) {
        messages__menu(app);
        return;
    }
    if (app->ui.screen == UI_SCREEN_THREAD) {
        if (code == BRUCE_INPUT_CODE_BACK) messages__close(app);
        else if (code == BRUCE_INPUT_CODE_SELECT) messages__send(app);
        else if (code == '\b' || code == BRUCE_INPUT_CODE_DELETE) messages__backspace(app);
        else if (code == BRUCE_INPUT_CODE_UP) messages__scroll(app, ui__thread_step(), width, height);
        else if (code == BRUCE_INPUT_CODE_DOWN) messages__scroll(app, -ui__thread_step(), width, height);
        else if (code >= 0x20 && code < 0x7F) messages__type(app, (char)code);
        return;
    }
    int rows = (int)app->ui.thread_count + 1;
    switch (code) {
        case BRUCE_INPUT_CODE_BACK: app->running = false; break;
        case BRUCE_INPUT_CODE_UP:
        case BRUCE_INPUT_CODE_PREV:
            app->ui.selected = (app->ui.selected + rows - 1) % rows;
            ui__scroll_to_selection(&app->ui, height);
            app->dirty = true;
            break;
        case BRUCE_INPUT_CODE_DOWN:
        case BRUCE_INPUT_CODE_NEXT:
            app->ui.selected = (app->ui.selected + 1) % rows;
            ui__scroll_to_selection(&app->ui, height);
            app->dirty = true;
            break;
        case BRUCE_INPUT_CODE_SELECT: messages__open_row(app, app->ui.selected); break;
        default: break;
    }
}

static void messages__touch(messages_t *app, const bruce_input_event_t *event, int width, int height) {
    if (event->action != BRUCE_INPUT_PRESS) return;
    ui_hit_t hit = ui__hit(&app->ui, width, height, event->x, event->y);
    switch (hit.kind) {
        case UI_HIT_ROW:
            if (hit.index > 0 && input__wait_long_press(event, MESSAGES_LONG_PRESS_MS)) {
                app->ui.selected = hit.index;
                messages__menu(app);
            } else {
                messages__open_row(app, hit.index);
            }
            break;
        case UI_HIT_BACK: messages__close(app); break;
        case UI_HIT_COMPOSE: messages__compose(app); break;
        case UI_HIT_SEND: messages__send(app); break;
        case UI_HIT_NONE: break;
    }
}

static bool messages__draw(messages_t *app) {
    if (display__width() <= 0 || display__height() <= 0 || display__begin_frame() != BRUCE_OK) return false;
    dialog__draw_window(&app->area);
    ui__set_origin(app->area.x, app->area.y);
    ui__draw(&app->ui, app->area.width, app->area.height);
    display__present();
    app->drawn_at = runtime__now();
    app->dirty = false;
    return true;
}

static void messages__init(messages_t *app, store_t *store, const char *number) {
    memset(app, 0, sizeof(*app));
    app->store = store;
    app->ui.store = store;
    app->ui.epaper = display__is_epaper();
    app->running = true;
    store__load(store);
    messages__refresh_threads(app);
    messages__poll(app, true);
    /* Messages that came while the app was closed are still on the SIM. */
    if (app->ui.status.state == BRUCE_CELLULAR_READY) messages__sync(app);
    if (number != NULL && number[0] != '\0') messages__open(app, number);
}

static void messages__run(messages_t *app) {
    ui__init_metrics();
    while (app->running) {
        /* Blocks while minimised, until it is shown again or tiled in the preview. */
        if (process__is_background() && display__await_redraw()) app->dirty = true;
        bruce_input_event_t event;
        while (app->running && input__poll(&event) == BRUCE_OK) {
            if (event.type == BRUCE_INPUT_TOUCH) {
                bruce_input_event_t local = event;
                local.x = (int16_t)(event.x - app->area.x);
                local.y = (int16_t)(event.y - app->area.y);
                messages__touch(app, &local, app->area.width, app->area.height);
            } else if (event.action == BRUCE_INPUT_PRESS) {
                messages__key(app, event.code, app->area.width, app->area.height);
            }
        }
        messages__poll(app, false);
        if (display__viewport_changed()) app->dirty = true;
        /* Keeps the frame's clock and status icons current. */
        uint32_t refresh = app->area.refresh_interval_ms;
        if (refresh > 0 && runtime__now() - app->drawn_at >= refresh) app->dirty = true;
        if (!app->dirty || !messages__draw(app)) runtime__delay(MESSAGES_IDLE_DELAY_MS);
    }
}

int app_main(int argc, char **argv) {
    if (!cellular__is_available()) {
        dialog__message(BRUCE_DIALOG_ERROR, "Messages", "This device has no cellular modem");
        return 1;
    }
    messages_t *app = memory__calloc(1, sizeof(messages_t));
    store_t *store = memory__calloc(1, sizeof(store_t));
    if (app == NULL || store == NULL) {
        memory__free(app);
        memory__free(store);
        dialog__message(BRUCE_DIALOG_ERROR, "Messages", "Not enough memory");
        return 1;
    }
    messages__init(app, store, argc > 1 ? argv[1] : NULL);
    messages__run(app);
    memory__free(store);
    memory__free(app);
    return 0;
}
