/* The app's input and polling logic, compiled straight from phone.c. */
#include "../main/phone.c"

#include "fakes.h"
#include "test.h"

static phone_t app;
static book_t book;

static void start(void) { phone__init(&app, &book); }

static void type(const char *keys) {
    for (const char *c = keys; *c != '\0'; ++c) phone__key(&app, *c, 320);
}

static void typing_and_calling(void) {
    start();
    type("60x1");
    CHECK_STR(app.ui.number, "6081");
    phone__key(&app, '\b', 320);
    CHECK_STR(app.ui.number, "608");
    phone__key(&app, BRUCE_INPUT_CODE_SELECT, 320);
    CHECK_STR(fakes.dialed, "608");
    CHECK_STR(app.ui.number, "");
    CHECK_EQ(app.ui.status.call, BRUCE_CALL_DIALING);
}

static void empty_call_brings_back_last_number(void) {
    fakes__set_file(BOOK_CALLS_FILE, "0,0,i,0,700\n0,0,o,0,112\n");
    start();
    phone__key(&app, BRUCE_INPUT_CODE_SELECT, 320);
    CHECK_STR(app.ui.number, "112");
    CHECK_STR(fakes.dialed, "");
}

static void service_codes_use_ussd(void) {
    start();
    fakes.ussd_reply = "Balance 5 PLN";
    type("*100#");
    phone__key(&app, BRUCE_INPUT_CODE_SELECT, 320);
    CHECK_STR(fakes.ussd_code, "*100#");
    CHECK_STR(fakes.dialed, "");
    CHECK_STR(fakes.message, "Balance 5 PLN");
}

static void dial_error_explains_state(void) {
    start();
    fakes.status.state = BRUCE_CELLULAR_NO_SIM;
    fakes.dial_result = BRUCE_ERR_INVALID_STATE;
    phone__poll(&app, true);
    phone__call(&app, "112");
    CHECK_STR(fakes.message, "No SIM card");
}

static void in_call_keys(void) {
    start();
    fakes.status.call = BRUCE_CALL_INCOMING;
    phone__poll(&app, true);
    phone__key(&app, BRUCE_INPUT_CODE_SELECT, 320);
    CHECK_EQ(fakes.answers, 1);
    type("12#");
    CHECK_STR(fakes.dtmf, "12#");
    CHECK_STR(app.ui.dtmf, "12#");
    phone__key(&app, 'm', 320);
    CHECK(fakes.status.muted);
    phone__key(&app, BRUCE_INPUT_CODE_UP, 320);
    CHECK_EQ(fakes.status.volume, 80);
    phone__key(&app, BRUCE_INPUT_CODE_BACK, 320);
    CHECK_EQ(fakes.hang_ups, 1);
    CHECK(app.running);
}

static void touch_answer_and_keypad(void) {
    start();
    fakes.status.call = BRUCE_CALL_INCOMING;
    phone__poll(&app, true);
    phone__press(&app, (ui_hit_t){UI_HIT_ANSWER, 0});
    CHECK_EQ(fakes.answers, 1);
    phone__press(&app, (ui_hit_t){UI_HIT_DTMF_PAD, 0});
    CHECK(app.ui.dtmf_pad);
    phone__press(&app, (ui_hit_t){UI_HIT_KEY, 10});
    CHECK_STR(fakes.dtmf, "0");
    phone__press(&app, (ui_hit_t){UI_HIT_HANG_UP, 0});
    CHECK(!app.ui.dtmf_pad);
    CHECK_STR(app.ui.dtmf, "");
}

static void missed_calls_from_the_modem_log_are_saved(void) {
    fakes__log(true, BRUCE_CALL_RESULT_MISSED, "600100200", fakes.now_utc - 60, 0);
    start();
    CHECK_EQ(book.call_count, 1);
    CHECK_EQ(app.ui.tab, UI_TAB_RECENTS);
    CHECK(strstr(fakes__file(BOOK_CALLS_FILE)->data, ",i,1,600100200") != NULL);
    fakes__log(false, BRUCE_CALL_RESULT_ANSWERED, "112", fakes.now_utc, 30);
    fakes.now_ms += 1000;
    phone__poll(&app, false);
    CHECK_EQ(book.call_count, 2);
    CHECK_STR(book.calls[0].number, "112");
    /* Opening the app again in the same boot does not add them twice. */
    start();
    CHECK_EQ(book.call_count, 2);
}

static void phone_is_turned_on_at_start(void) {
    fakes.status.state = BRUCE_CELLULAR_OFF;
    start();
    CHECK_EQ(fakes.enables, 1);
    CHECK(fakes.enabled);
}

static void lists_navigate_and_call(void) {
    fakes__set_file(BOOK_CONTACTS_FILE, "111,Ann\n222,Bob\n");
    start();
    phone__key(&app, BRUCE_INPUT_CODE_RIGHT, 320);
    phone__key(&app, BRUCE_INPUT_CODE_RIGHT, 320);
    CHECK_EQ(app.ui.tab, UI_TAB_CONTACTS);
    phone__key(&app, BRUCE_INPUT_CODE_DOWN, 320);
    phone__key(&app, BRUCE_INPUT_CODE_DOWN, 320);
    CHECK_EQ(app.ui.selected, 2);
    phone__key(&app, BRUCE_INPUT_CODE_SELECT, 320);
    CHECK_STR(fakes.dialed, "222");
}

static void new_contact_from_keypad(void) {
    start();
    type("555");
    fakes.text_inputs[0] = "Carl";
    phone__press(&app, (ui_hit_t){UI_HIT_SAVE_NUMBER, 0});
    CHECK_EQ(book.contact_count, 1);
    CHECK_STR(book.contacts[0].name, "Carl");
    CHECK_STR(fakes__file(BOOK_CONTACTS_FILE)->data, "555,Carl\n");
}

static void menu_deletes_a_contact(void) {
    fakes__set_file(BOOK_CONTACTS_FILE, "111,Ann\n");
    start();
    app.ui.tab = UI_TAB_CONTACTS;
    app.ui.selected = 1;
    /* Call, Send message, Edit contact, Delete, ...; then Cancel in the confirmation. */
    fakes.choices[0] = 3;
    fakes.choice_queue = 1;
    phone__menu(&app);
    CHECK_STR(fakes.choice_labels[1], "Delete");
    CHECK_EQ(book.contact_count, 1);
    fakes.choices[0] = 3;
    fakes.choices[1] = 1;
    fakes.choice_queue = 2;
    fakes.choice_next = 0;
    phone__menu(&app);
    CHECK_EQ(book.contact_count, 0);
    CHECK_STR(fakes__file(BOOK_CONTACTS_FILE)->data, "");
    CHECK_EQ(app.ui.selected, 0);
}

static void menu_opens_messages(void) {
    fakes__set_file(BOOK_CONTACTS_FILE, "600100200,Ann\n");
    start();
    app.ui.tab = UI_TAB_CONTACTS;
    app.ui.selected = 1;
    fakes.choice = 1;
    phone__menu(&app);
    CHECK_STR(fakes.command, "GUI=1 messages 600100200");
}

static void keypad_letters_map_to_digits(void) {
    CHECK_EQ(phone__keypad_char('w'), '1');
    CHECK_EQ(phone__keypad_char('C'), '9');
    CHECK_EQ(phone__keypad_char('7'), '7');
    CHECK_EQ(phone__keypad_char('h'), 0);
}

static void back_exits(void) {
    start();
    phone__key(&app, BRUCE_INPUT_CODE_BACK, 320);
    CHECK(!app.running);
}

static void paper_timer_redraws_every_15_seconds(void) {
    fakes.epaper = true;
    start();
    fakes.status.call = BRUCE_CALL_ACTIVE;
    fakes.status.call_seconds = 1;
    phone__poll(&app, true);
    app.dirty = false;
    fakes.status.call_seconds = 10;
    phone__poll(&app, true);
    CHECK(!app.dirty);
    fakes.status.call_seconds = 16;
    phone__poll(&app, true);
    CHECK(app.dirty);
}

SUITE(
    phone_tests, TEST(typing_and_calling), TEST(empty_call_brings_back_last_number),
    TEST(service_codes_use_ussd), TEST(dial_error_explains_state), TEST(in_call_keys),
    TEST(touch_answer_and_keypad), TEST(missed_calls_from_the_modem_log_are_saved),
    TEST(phone_is_turned_on_at_start), TEST(lists_navigate_and_call), TEST(new_contact_from_keypad),
    TEST(menu_deletes_a_contact), TEST(keypad_letters_map_to_digits),
    TEST(paper_timer_redraws_every_15_seconds), TEST(back_exits), TEST(menu_opens_messages)
)
