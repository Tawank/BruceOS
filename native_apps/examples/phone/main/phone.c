#include <stdio.h>
#include <string.h>

#include "book.h"
#include "bruce_sdk.h"
#include "ui.h"

#define PHONE_IDLE_DELAY_MS 30
#define PHONE_POLL_MS 500
#define PHONE_LONG_PRESS_MS 500
#define PHONE_USSD_TIMEOUT_MS 30000
#define PHONE_VOLUME_STEP 20
/* E-paper redraws the call timer this often instead of every second. */
#define PHONE_PAPER_TIMER_S 15

typedef struct {
    ui_t ui;
    book_t *book;
    bool running;
    bool dirty;
    uint64_t polled_at;
    bruce_dialog_window_area_t area; /* Inside the launcher's window frame. */
    uint64_t drawn_at;
    uint32_t log_sequence; /* Newest modem call log record already in the book. */
} phone_t;

static void phone__poll(phone_t *app, bool now);

static bool phone__dial_char(char c) { return (c >= '0' && c <= '9') || c == '*' || c == '#' || c == '+'; }

/* The T-Deck keyboard prints the keypad on its letter keys (Alt types them). */
static char phone__keypad_char(int32_t code) {
    static const char map[][2] = {
        {'w', '1'},
        {'e', '2'},
        {'r', '3'},
        {'s', '4'},
        {'d', '5'},
        {'f', '6'},
        {'z', '7'},
        {'x', '8'},
        {'c', '9'},
        {'q', '#'},
        {'a', '*'},
        {'`', '0'},
        {'o', '+'},
    };
    if (code > 0 && code < 128 && phone__dial_char((char)code)) return (char)code;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (code == map[i][0] || code == map[i][0] - 'a' + 'A') return map[i][1];
    }
    return 0;
}

static int64_t phone__now_utc(void) {
    bruce_clock_datetime_t now;
    int64_t epoch = 0;
    if (clock__get_utc(&now) != BRUCE_OK || clock__datetime_to_epoch(&now, &epoch) != BRUCE_OK) return 0;
    return epoch;
}

static void phone__append(char *text, size_t capacity, char c) {
    size_t length = strlen(text);
    if (length + 1 < capacity) {
        text[length] = c;
        text[length + 1] = '\0';
    }
}

static void phone__backspace(phone_t *app) {
    size_t length = strlen(app->ui.number);
    if (length > 0) app->ui.number[length - 1] = '\0';
    app->dirty = true;
}

static void phone__error(bruce_result_t result, bruce_cellular_state_t state) {
    const char *text = result == BRUCE_ERR_INVALID_STATE && state != BRUCE_CELLULAR_READY
                           ? ui__state_text(state)
                       : result == BRUCE_ERR_BUSY    ? "Already in a call"
                       : result == BRUCE_ERR_TIMEOUT ? "No reply from the network"
                                                     : result__to_string(result);
    dialog__message(BRUCE_DIALOG_ERROR, "Phone", text);
}

static void phone__ussd(phone_t *app, const char *code) {
    dialog__message_show(BRUCE_DIALOG_INFO, code, "Sending...");
    char reply[192];
    bruce_result_t result = cellular__ussd(code, reply, sizeof(reply), PHONE_USSD_TIMEOUT_MS);
    if (result == BRUCE_OK) dialog__message(BRUCE_DIALOG_INFO, code, reply[0] != '\0' ? reply : "Done");
    else phone__error(result, app->ui.status.state);
    app->dirty = true;
}

static void phone__call(phone_t *app, const char *number) {
    if (number == NULL || number[0] == '\0') return;
    char copy[BRUCE_CELLULAR_NUMBER_MAX];
    snprintf(copy, sizeof(copy), "%s", number);
    if (book__is_ussd(copy)) {
        phone__ussd(app, copy);
        return;
    }
    bruce_result_t result = cellular__dial(copy);
    if (result != BRUCE_OK) {
        phone__error(result, app->ui.status.state);
    } else {
        app->ui.number[0] = '\0';
        app->ui.dtmf[0] = '\0';
        app->ui.dtmf_pad = false;
        phone__poll(app, true);
    }
    app->dirty = true;
}

/* Call, or with nothing typed bring back the last number called. */
static void phone__call_typed(phone_t *app) {
    if (app->ui.number[0] != '\0') {
        phone__call(app, app->ui.number);
        return;
    }
    for (size_t i = 0; i < app->book->call_count; ++i) {
        if (!app->book->calls[i].incoming) {
            snprintf(app->ui.number, sizeof(app->ui.number), "%s", app->book->calls[i].number);
            app->dirty = true;
            return;
        }
    }
}

static void phone__set_tab(phone_t *app, ui_tab_t tab) {
    app->ui.tab = tab;
    app->ui.selected = 0;
    app->ui.scroll = 0;
    app->dirty = true;
}

static bool
phone__ask(const char *title, const char *prompt, const char *initial, char *out, size_t capacity) {
    return dialog__text_input(title, prompt, initial, false, out, capacity) == BRUCE_OK;
}

static bool phone__valid_number(const char *number) {
    if (number[0] == '\0') return false;
    for (const char *c = number; *c != '\0'; ++c) {
        if (!phone__dial_char(*c) || (*c == '+' && c != number)) return false;
    }
    return true;
}

static void phone__new_contact(phone_t *app, const char *number) {
    char typed[BRUCE_CELLULAR_NUMBER_MAX] = "";
    if (number == NULL || number[0] == '\0') {
        if (!phone__ask("New contact", "Number", NULL, typed, sizeof(typed))) return;
        number = typed;
    }
    if (!phone__valid_number(number)) {
        dialog__message(BRUCE_DIALOG_ERROR, "New contact", "Use digits, * and #, with + only first");
        return;
    }
    char name[BOOK_NAME_MAX] = "";
    if (!phone__ask("New contact", "Name", NULL, name, sizeof(name))) return;
    int index = book__add_contact(app->book, name, number);
    if (index < 0) {
        dialog__message(BRUCE_DIALOG_ERROR, "New contact", "The contact list is full");
        return;
    }
    if (!book__save_contacts(app->book))
        dialog__message(BRUCE_DIALOG_ERROR, "Phone", "Could not save contacts");
    if (app->ui.tab == UI_TAB_CONTACTS) app->ui.selected = index + 1;
}

static void phone__edit_contact(phone_t *app, size_t index) {
    book_contact_t contact = app->book->contacts[index];
    char name[BOOK_NAME_MAX], number[BRUCE_CELLULAR_NUMBER_MAX];
    if (!phone__ask("Edit contact", "Name", contact.name, name, sizeof(name)) ||
        !phone__ask("Edit contact", "Number", contact.number, number, sizeof(number))) {
        return;
    }
    if (!phone__valid_number(number)) {
        dialog__message(BRUCE_DIALOG_ERROR, "Edit contact", "Use digits, * and #, with + only first");
        return;
    }
    book__remove_contact(app->book, index);
    int added = book__add_contact(app->book, name, number);
    if (added >= 0) app->ui.selected = added + 1;
    book__save_contacts(app->book);
}

static bool phone__confirm(const char *title, const char *message, const char *action) {
    bruce_dialog_choice_t choices[] = {
        {.label = "Cancel"},
        {.label = action},
    };
    size_t selected = 0;
    return dialog__choice(title, message, choices, 2, &selected) == BRUCE_OK && selected == 1;
}

typedef enum {
    PHONE_MENU_CALL,
    PHONE_MENU_SAVE,
    PHONE_MENU_EDIT,
    PHONE_MENU_DELETE,
    PHONE_MENU_CLEAR,
    PHONE_MENU_POWER,
    PHONE_MENU_EXIT,
    PHONE_MENU_BACK,
    PHONE_MENU_COUNT,
} phone_menu_t;

/* Options for the selected row, plus turning the phone on or off. */
static void phone__menu(phone_t *app) {
    const book_t *book = app->book;
    int row = app->ui.selected;
    bool on_call = app->ui.tab == UI_TAB_RECENTS && row >= 0 && row < (int)book->call_count;
    bool on_contact = app->ui.tab == UI_TAB_CONTACTS && row > 0 && row <= (int)book->contact_count;
    const char *number = on_call      ? book->calls[row].number
                         : on_contact ? book->contacts[row - 1].number
                                      : app->ui.number;
    bool off = app->ui.status.state == BRUCE_CELLULAR_OFF;

    bruce_dialog_choice_t choices[PHONE_MENU_COUNT];
    phone_menu_t actions[PHONE_MENU_COUNT];
    size_t count = 0;
#define PHONE_ADD(action, text)                                                                              \
    do {                                                                                                     \
        choices[count] = (bruce_dialog_choice_t){.label = (text)};                                           \
        actions[count++] = (action);                                                                         \
    } while (0)
    if ((on_call || on_contact || app->ui.tab == UI_TAB_KEYPAD) && number[0] != '\0')
        PHONE_ADD(PHONE_MENU_CALL, "Call");
    if ((on_call || app->ui.tab == UI_TAB_KEYPAD) && number[0] != '\0' && !book__is_ussd(number) &&
        book__name_for(book, number) == NULL) {
        PHONE_ADD(PHONE_MENU_SAVE, "Add to contacts");
    }
    if (on_contact) PHONE_ADD(PHONE_MENU_EDIT, "Edit contact");
    if (on_call || on_contact) PHONE_ADD(PHONE_MENU_DELETE, "Delete");
    if (app->ui.tab == UI_TAB_RECENTS && book->call_count > 0) PHONE_ADD(PHONE_MENU_CLEAR, "Clear recents");
    PHONE_ADD(PHONE_MENU_POWER, off ? "Turn phone on" : "Turn phone off");
    PHONE_ADD(PHONE_MENU_EXIT, "Exit");
    PHONE_ADD(PHONE_MENU_BACK, "Back");
#undef PHONE_ADD

    size_t selected = 0;
    app->dirty = true;
    if (dialog__choice("Phone", NULL, choices, count, &selected) != BRUCE_OK) return;
    switch (actions[selected]) {
        case PHONE_MENU_CALL: phone__call(app, number); break;
        case PHONE_MENU_SAVE: phone__new_contact(app, number); break;
        case PHONE_MENU_EDIT: phone__edit_contact(app, (size_t)row - 1); break;
        case PHONE_MENU_DELETE:
            if (on_call) {
                book__remove_call(app->book, (size_t)row);
                book__save_calls(app->book);
            } else if (phone__confirm("Delete contact", book->contacts[row - 1].name, "Delete")) {
                book__remove_contact(app->book, (size_t)row - 1);
                book__save_contacts(app->book);
            }
            break;
        case PHONE_MENU_CLEAR:
            if (phone__confirm("Clear recents", "Delete the whole call history?", "Clear")) {
                app->book->call_count = 0;
                book__save_calls(app->book);
            }
            break;
        case PHONE_MENU_POWER: cellular__set_enabled(off); break;
        case PHONE_MENU_EXIT: app->running = false; break;
        case PHONE_MENU_BACK: break;
        case PHONE_MENU_COUNT: break;
    }
    int rows = (int)ui__row_count(&app->ui);
    if (app->ui.selected >= rows) app->ui.selected = rows > 0 ? rows - 1 : 0;
}

static void phone__open_row(phone_t *app, int row) {
    app->ui.selected = row;
    if (app->ui.tab == UI_TAB_RECENTS && row < (int)app->book->call_count) {
        phone__call(app, app->book->calls[row].number);
    } else if (app->ui.tab == UI_TAB_CONTACTS) {
        if (row == 0) phone__new_contact(app, NULL);
        else if (row <= (int)app->book->contact_count) phone__call(app, app->book->contacts[row - 1].number);
    }
    app->dirty = true;
}

static void phone__move(phone_t *app, int delta, int height) {
    int rows = (int)ui__row_count(&app->ui);
    if (rows == 0) return;
    app->ui.selected = (app->ui.selected + delta + rows) % rows;
    ui__scroll_to_selection(&app->ui, height);
    app->dirty = true;
}

static void phone__dtmf(phone_t *app, char digit) {
    if (digit == '+') return;
    if (cellular__send_dtmf(digit) == BRUCE_OK) phone__append(app->ui.dtmf, sizeof(app->ui.dtmf), digit);
    app->dirty = true;
}

static void phone__volume(phone_t *app, int delta) {
    int volume = app->ui.status.volume + delta;
    if (delta > 0 && volume > 100) volume = PHONE_VOLUME_STEP;
    if (volume < 0) volume = 0;
    cellular__set_volume((uint8_t)(volume > 100 ? 100 : volume));
    phone__poll(app, true);
}

static void phone__press(phone_t *app, ui_hit_t hit) {
    switch (hit.kind) {
        case UI_HIT_TAB: phone__set_tab(app, (ui_tab_t)hit.index); break;
        case UI_HIT_KEY:
            if (ui__in_call(&app->ui)) {
                phone__dtmf(app, ui__key_char(hit.index));
            } else {
                phone__append(app->ui.number, sizeof(app->ui.number), ui__key_char(hit.index));
                app->dirty = true;
            }
            break;
        case UI_HIT_BACKSPACE: phone__backspace(app); break;
        case UI_HIT_CALL: phone__call_typed(app); break;
        case UI_HIT_SAVE_NUMBER: phone__new_contact(app, app->ui.number); break;
        case UI_HIT_ROW: phone__open_row(app, hit.index); break;
        case UI_HIT_ANSWER: cellular__answer(); break;
        case UI_HIT_DECLINE:
        case UI_HIT_HANG_UP: cellular__hang_up(); break;
        case UI_HIT_MUTE: cellular__set_muted(!app->ui.status.muted); break;
        case UI_HIT_DTMF_PAD: app->ui.dtmf_pad = !app->ui.dtmf_pad; break;
        case UI_HIT_VOLUME: phone__volume(app, PHONE_VOLUME_STEP); break;
        case UI_HIT_NONE: return;
    }
    phone__poll(app, true);
    app->dirty = true;
}

static void phone__touch(phone_t *app, const bruce_input_event_t *event, int width, int height) {
    if (event->action != BRUCE_INPUT_PRESS) return;
    ui_hit_t hit = ui__hit(&app->ui, width, height, event->x, event->y);
    if (hit.kind == UI_HIT_ROW || (hit.kind == UI_HIT_BACKSPACE)) {
        if (input__wait_long_press(event, PHONE_LONG_PRESS_MS)) {
            if (hit.kind == UI_HIT_BACKSPACE) {
                app->ui.number[0] = '\0';
                app->dirty = true;
            } else {
                app->ui.selected = hit.index;
                phone__menu(app);
            }
            return;
        }
    }
    phone__press(app, hit);
}

static void phone__key_in_call(phone_t *app, int32_t code) {
    bool incoming = app->ui.status.call == BRUCE_CALL_INCOMING;
    if (incoming) {
        if (code == BRUCE_INPUT_CODE_SELECT) cellular__answer();
        else if (code == BRUCE_INPUT_CODE_BACK) cellular__hang_up();
    } else if (code == BRUCE_INPUT_CODE_BACK || code == BRUCE_INPUT_CODE_DELETE) {
        cellular__hang_up();
    } else if (code == BRUCE_INPUT_CODE_UP || code == '=') {
        phone__volume(app, PHONE_VOLUME_STEP);
    } else if (code == BRUCE_INPUT_CODE_DOWN || code == '-') {
        phone__volume(app, -PHONE_VOLUME_STEP);
    } else if (code == 'm' || code == 'M') {
        cellular__set_muted(!app->ui.status.muted);
    } else if (phone__keypad_char(code) != 0) {
        phone__dtmf(app, phone__keypad_char(code));
    }
    phone__poll(app, true);
    app->dirty = true;
}

static void phone__key(phone_t *app, int32_t code, int height) {
    if (ui__in_call(&app->ui)) {
        phone__key_in_call(app, code);
        return;
    }
    switch (code) {
        case BRUCE_INPUT_CODE_BACK: app->running = false; return;
        case BRUCE_INPUT_CODE_MENU: phone__menu(app); return;
        case BRUCE_INPUT_CODE_LEFT:
            phone__set_tab(app, (ui_tab_t)((app->ui.tab + UI_TAB_COUNT - 1) % UI_TAB_COUNT));
            return;
        case BRUCE_INPUT_CODE_RIGHT:
            phone__set_tab(app, (ui_tab_t)((app->ui.tab + 1) % UI_TAB_COUNT));
            return;
        default: break;
    }
    if (app->ui.tab == UI_TAB_KEYPAD) {
        if (code == '\b' || code == BRUCE_INPUT_CODE_DELETE) phone__backspace(app);
        else if (code == BRUCE_INPUT_CODE_SELECT) phone__call_typed(app);
        else if (phone__keypad_char(code) != 0) {
            phone__append(app->ui.number, sizeof(app->ui.number), phone__keypad_char(code));
            app->dirty = true;
        }
        return;
    }
    switch (code) {
        case BRUCE_INPUT_CODE_UP:
        case BRUCE_INPUT_CODE_PREV: phone__move(app, -1, height); break;
        case BRUCE_INPUT_CODE_DOWN:
        case BRUCE_INPUT_CODE_NEXT: phone__move(app, 1, height); break;
        case BRUCE_INPUT_CODE_SELECT: phone__open_row(app, app->ui.selected); break;
        default:
            /* Typing a number on a list starts dialling it. */
            if (code > 0 && code < 128 && phone__dial_char((char)code)) {
                phone__set_tab(app, UI_TAB_KEYPAD);
                phone__append(app->ui.number, sizeof(app->ui.number), (char)code);
            }
            break;
    }
}

/* Adds calls the modem logged since the last look, e.g. ones missed while the app was closed. */
static void phone__sync_log(phone_t *app) {
    bruce_cellular_call_record_t records[BRUCE_CELLULAR_LOG_MAX];
    size_t count = cellular__get_call_log(app->log_sequence, records, BRUCE_CELLULAR_LOG_MAX);
    bool added = false;
    for (size_t i = 0; i < count; ++i) {
        added |= book__add_call(app->book, &records[i]);
        if (records[i].sequence > app->log_sequence) app->log_sequence = records[i].sequence;
    }
    if (added) {
        book__save_calls(app->book);
        app->dirty = true;
    }
}

static bool
phone__status_changed(const bruce_cellular_status_t *a, const bruce_cellular_status_t *b, bool epaper) {
    if (a->state != b->state || a->signal_bars != b->signal_bars || a->call != b->call ||
        a->muted != b->muted || a->volume != b->volume || strcmp(a->operator_name, b->operator_name) != 0 ||
        strcmp(a->call_number, b->call_number) != 0) {
        return true;
    }
    if (a->call != BRUCE_CALL_ACTIVE) return false;
    uint32_t step = epaper ? PHONE_PAPER_TIMER_S : 1;
    return a->call_seconds / step != b->call_seconds / step;
}

static void phone__poll(phone_t *app, bool now) {
    uint64_t time = runtime__now();
    if (!now && time - app->polled_at < PHONE_POLL_MS) return;
    app->polled_at = time;
    bruce_cellular_status_t status;
    if (cellular__get_status(&status) != BRUCE_OK) return;
    bool was_in_call = app->ui.status.call != BRUCE_CALL_NONE;
    if (phone__status_changed(&status, &app->ui.status, app->ui.epaper)) app->dirty = true;
    app->ui.status = status;
    if (was_in_call && status.call == BRUCE_CALL_NONE) {
        app->ui.dtmf_pad = false;
        app->ui.dtmf[0] = '\0';
    }
    if (status.last_log_sequence != app->log_sequence) phone__sync_log(app);
    int64_t utc = phone__now_utc();
    if (utc / 60 != app->ui.now_utc / 60) {
        app->ui.now_utc = utc;
        app->ui.utc_offset = clock__get_local_offset_seconds();
    }
}

static bool phone__init(phone_t *app, book_t *book) {
    memset(app, 0, sizeof(*app));
    app->book = book;
    app->ui.book = book;
    app->ui.epaper = display__is_epaper();
    app->ui.status.signal_bars = -1;
    app->running = true;
    app->dirty = true;
    book__load(book);
    bruce_cellular_status_t status;
    if (cellular__get_status(&status) == BRUCE_OK && status.state == BRUCE_CELLULAR_OFF)
        cellular__set_enabled(true);
    phone__poll(app, true);
    if (app->book->call_count > 0 && app->book->calls[0].result == BRUCE_CALL_RESULT_MISSED) {
        app->ui.tab = UI_TAB_RECENTS;
    }
    return true;
}

static bool phone__draw(phone_t *app) {
    if (display__width() <= 0 || display__height() <= 0 || display__begin_frame() != BRUCE_OK) return false;
    dialog__draw_window(&app->area);
    ui__set_origin(app->area.x, app->area.y);
    ui__draw(&app->ui, app->area.width, app->area.height);
    display__present();
    app->drawn_at = runtime__now();
    app->dirty = false;
    return true;
}

static void phone__run(phone_t *app) {
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
                phone__touch(app, &local, app->area.width, app->area.height);
            } else if (event.action == BRUCE_INPUT_PRESS) {
                phone__key(app, event.code, app->area.height);
            }
        }
        phone__poll(app, false);
        if (display__viewport_changed()) app->dirty = true;
        /* Keeps the frame's clock and status icons current. */
        uint32_t refresh = app->area.refresh_interval_ms;
        if (refresh > 0 && runtime__now() - app->drawn_at >= refresh) app->dirty = true;
        if (!app->dirty || !phone__draw(app)) runtime__delay(PHONE_IDLE_DELAY_MS);
    }
}

int app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!cellular__is_available()) {
        dialog__message(BRUCE_DIALOG_ERROR, "Phone", "This device has no cellular modem");
        return 1;
    }
    phone_t *app = memory__calloc(1, sizeof(phone_t));
    book_t *book = memory__calloc(1, sizeof(book_t));
    if (app == NULL || book == NULL) {
        memory__free(app);
        memory__free(book);
        dialog__message(BRUCE_DIALOG_ERROR, "Phone", "Not enough memory");
        return 1;
    }
    phone__init(app, book);
    phone__run(app);
    memory__free(book);
    memory__free(app);
    return 0;
}
