#include "fakes.h"
#include "test.h"
#include "ui.h"

static book_t book;
static ui_t ui;

static void start(void) {
    memset(&book, 0, sizeof(book));
    memset(&ui, 0, sizeof(ui));
    ui.book = &book;
    ui.status = fakes.status;
    ui.now_utc = fakes.now_utc;
    ui.utc_offset = 3600;
    ui__init_metrics();
}

static int centre_x(ui_rect_t r) { return r.x + r.w / 2; }
static int centre_y(ui_rect_t r) { return r.y + r.h / 2; }

static void keys_fit_between_display_and_tabs(void) {
    start();
    ui_rect_t first = ui__key_rect(0, 240, 320);
    ui_rect_t last = ui__key_rect(11, 240, 320);
    ui_rect_t call = ui__call_button_rect(240, 320);
    ui_rect_t tab = ui__tab_rect(0, 240, 320);
    CHECK(first.y > ui__backspace_rect(240, 320).y);
    CHECK(last.y + last.h <= call.y);
    CHECK(call.y + call.h <= tab.y);
    CHECK(last.x + last.w <= 240);
    CHECK(first.h >= 30);
}

static void hit_finds_keys_tabs_and_call(void) {
    start();
    ui_rect_t five = ui__key_rect(4, 240, 320);
    ui_hit_t hit = ui__hit(&ui, 240, 320, centre_x(five), centre_y(five));
    CHECK_EQ(hit.kind, UI_HIT_KEY);
    CHECK_EQ(ui__key_char(hit.index), '5');
    ui_rect_t call = ui__call_button_rect(240, 320);
    CHECK_EQ(ui__hit(&ui, 240, 320, centre_x(call), centre_y(call)).kind, UI_HIT_CALL);
    ui_rect_t contacts = ui__tab_rect(UI_TAB_CONTACTS, 240, 320);
    hit = ui__hit(&ui, 240, 320, centre_x(contacts), centre_y(contacts));
    CHECK_EQ(hit.kind, UI_HIT_TAB);
    CHECK_EQ(hit.index, UI_TAB_CONTACTS);
    ui_rect_t back = ui__backspace_rect(240, 320);
    CHECK_EQ(ui__hit(&ui, 240, 320, centre_x(back), centre_y(back)).kind, UI_HIT_NONE);
    snprintf(ui.number, sizeof(ui.number), "12");
    CHECK_EQ(ui__hit(&ui, 240, 320, centre_x(back), centre_y(back)).kind, UI_HIT_BACKSPACE);
}

static void call_screen_buttons(void) {
    start();
    ui.status.call = BRUCE_CALL_INCOMING;
    ui_rect_t answer = ui__call_screen_rect(UI_HIT_ANSWER, true, 240, 320);
    ui_rect_t decline = ui__call_screen_rect(UI_HIT_DECLINE, true, 240, 320);
    CHECK_EQ(ui__hit(&ui, 240, 320, centre_x(answer), centre_y(answer)).kind, UI_HIT_ANSWER);
    CHECK_EQ(ui__hit(&ui, 240, 320, centre_x(decline), centre_y(decline)).kind, UI_HIT_DECLINE);
    ui.status.call = BRUCE_CALL_ACTIVE;
    ui_rect_t hang_up = ui__call_screen_rect(UI_HIT_HANG_UP, false, 240, 320);
    CHECK_EQ(ui__hit(&ui, 240, 320, centre_x(hang_up), centre_y(hang_up)).kind, UI_HIT_HANG_UP);
    ui.dtmf_pad = true;
    ui_rect_t hash = ui__dtmf_key_rect(11, 240, 320);
    ui_rect_t mute = ui__call_screen_rect(UI_HIT_MUTE, false, 240, 320);
    CHECK(hash.y + hash.h < mute.y);
    ui_hit_t hit = ui__hit(&ui, 240, 320, centre_x(hash), centre_y(hash));
    CHECK_EQ(hit.kind, UI_HIT_KEY);
    CHECK_EQ(hit.index, 11);
}

static void draws_incoming_call_with_contact_name(void) {
    start();
    book__add_contact(&book, "Mum", "+48600100200");
    ui.status.call = BRUCE_CALL_INCOMING;
    snprintf(ui.status.call_number, sizeof(ui.status.call_number), "600100200");
    fakes.draw_count = 0;
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_string("Mum") != NULL);
    CHECK(fakes__find_string("600100200") != NULL);
    CHECK(fakes__find_string("Incoming call") != NULL);
    CHECK(fakes__find_icon("phone-hangup") != NULL);
    CHECK(fakes__find_icon("dialpad") == NULL);
}

static void draws_active_call_timer(void) {
    start();
    ui.status.call = BRUCE_CALL_ACTIVE;
    ui.status.call_seconds = 75;
    snprintf(ui.status.call_number, sizeof(ui.status.call_number), "112");
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_string("1:15") != NULL);
    CHECK(fakes__find_icon("microphone") != NULL);
    CHECK(fakes__find_string("60%") != NULL);
}

static void draws_recents_rows(void) {
    start();
    ui.tab = UI_TAB_RECENTS;
    book__add_contact(&book, "Anna", "600100200");
    bruce_cellular_call_record_t missed = {
        .incoming = true, .result = BRUCE_CALL_RESULT_MISSED, .started_utc = fakes.now_utc - 600
    };
    snprintf(missed.number, sizeof(missed.number), "600100200");
    bruce_cellular_call_record_t out = {
        .result = BRUCE_CALL_RESULT_ANSWERED, .seconds = 130, .started_utc = fakes.now_utc - 86400
    };
    snprintf(out.number, sizeof(out.number), "112");
    book__add_call(&book, &out);
    book__add_call(&book, &missed);
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_icon("phone-missed") != NULL);
    CHECK(fakes__find_string("Anna") != NULL);
    CHECK(fakes__find_string("Missed  600100200") != NULL);
    CHECK(fakes__find_string("05:30") != NULL);
    CHECK(fakes__find_string("2:10") != NULL);
    CHECK(fakes__find_string("Yesterday") != NULL);
}

static void draws_status_bar(void) {
    start();
    ui.status.state = BRUCE_CELLULAR_NO_SIM;
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_string("No SIM card") != NULL);
    ui.status.state = BRUCE_CELLULAR_READY;
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_string("Orange") != NULL);
    CHECK(fakes__find_icon("signal-cellular-3") != NULL);
}

static void formats_times(void) {
    char text[16];
    ui__format_duration(59, text, sizeof(text));
    CHECK_STR(text, "0:59");
    ui__format_duration(3725, text, sizeof(text));
    CHECK_STR(text, "1:02:05");
    int64_t now = fakes.now_utc; /* Friday 05:40 local */
    ui__format_when(now - 3 * 86400, now, 3600, text, sizeof(text));
    CHECK_STR(text, "Tue");
    ui__format_when(now - 30 * 86400, now, 3600, text, sizeof(text));
    CHECK_STR(text, "22.10");
    ui__format_when(0, now, 3600, text, sizeof(text));
    CHECK_STR(text, "");
}

static void scroll_follows_selection(void) {
    start();
    ui.tab = UI_TAB_CONTACTS;
    for (int i = 0; i < 20; ++i) {
        char name[8];
        snprintf(name, sizeof(name), "N%02d", i);
        book__add_contact(&book, name, "1");
    }
    ui.selected = 15;
    ui__scroll_to_selection(&ui, 320);
    CHECK(ui.scroll > 0);
    CHECK(ui.selected < ui.scroll + ui__visible_rows(320));
    ui.selected = 0;
    ui__scroll_to_selection(&ui, 320);
    CHECK_EQ(ui.scroll, 0);
}

SUITE(
    ui_tests, TEST(keys_fit_between_display_and_tabs), TEST(hit_finds_keys_tabs_and_call),
    TEST(call_screen_buttons), TEST(draws_incoming_call_with_contact_name), TEST(draws_active_call_timer),
    TEST(draws_recents_rows), TEST(draws_status_bar), TEST(formats_times), TEST(scroll_follows_selection)
)
