/* The app's static functions, tested directly. */
#include "../main/messages.c"

#include <stdlib.h>

#include "fakes.h"
#include "test.h"

static messages_t app;
static store_t *store;

static void start(const char *number) {
    free(store);
    store = calloc(1, sizeof(store_t));
    messages__init(&app, store, number);
}

static void type(const char *text) {
    for (const char *c = text; *c != '\0'; ++c) messages__key(&app, *c, 240, 320);
}

static void opening_moves_sim_messages_into_the_store(void) {
    fakes__receive("+48600100200", "Hello", 1763690000);
    fakes__receive("Orange", "Your balance", 0);
    start(NULL);
    CHECK_EQ(fakes.sim_count, 0);
    CHECK_EQ(fakes.deletes, 2);
    CHECK_EQ(store->message_count, 2);
    CHECK(store->messages[0].incoming && store->messages[0].unread);
    CHECK_EQ(store->messages[0].utc, 1763690000);
    CHECK_EQ(store->messages[1].utc, fakes.now_utc); /* No time from the network. */
    CHECK(strstr(fakes__file(STORE_MESSAGES_FILE)->data, "Your balance") != NULL);
    CHECK_EQ(app.ui.thread_count, 2);
}

static void new_message_while_open_is_synced(void) {
    start(NULL);
    fakes__receive("600100200", "Ping", 0);
    messages__poll(&app, true);
    CHECK_EQ(store->message_count, 1);
    CHECK_EQ(fakes.sim_count, 0);
}

static void opening_a_conversation_marks_it_read(void) {
    fakes__set_file(STORE_MESSAGES_FILE, "5,i,u,600100200,hello\n");
    start(NULL);
    messages__key(&app, BRUCE_INPUT_CODE_DOWN, 240, 320);
    messages__key(&app, BRUCE_INPUT_CODE_SELECT, 240, 320);
    CHECK_EQ(app.ui.screen, UI_SCREEN_THREAD);
    CHECK_STR(app.ui.number, "600100200");
    CHECK(!store->messages[0].unread);
    CHECK(strstr(fakes__file(STORE_MESSAGES_FILE)->data, ",r,") != NULL);
    messages__key(&app, BRUCE_INPUT_CODE_BACK, 240, 320);
    CHECK_EQ(app.ui.screen, UI_SCREEN_LIST);
    CHECK(app.running);
    messages__key(&app, BRUCE_INPUT_CODE_BACK, 240, 320);
    CHECK(!app.running);
}

static void typing_and_sending(void) {
    start("600100200");
    CHECK_EQ(app.ui.screen, UI_SCREEN_THREAD);
    type("Hi there");
    messages__key(&app, '\b', 240, 320);
    CHECK_STR(app.ui.draft, "Hi ther");
    messages__key(&app, BRUCE_INPUT_CODE_SELECT, 240, 320);
    CHECK_STR(fakes.sent_number, "600100200");
    CHECK_STR(fakes.sent_text, "Hi ther");
    CHECK_STR(app.ui.draft, "");
    CHECK_EQ(store->message_count, 1);
    CHECK(!store->messages[0].incoming);
    CHECK(fakes__file(STORE_MESSAGES_FILE) != NULL);
}

static void failed_send_keeps_the_draft(void) {
    start("600100200");
    fakes.send_result = BRUCE_ERR_IO;
    type("Hi");
    messages__send(&app);
    CHECK_STR(app.ui.draft, "Hi");
    CHECK_EQ(store->message_count, 0);
    CHECK_EQ(fakes.message_count, 1);
    CHECK_STR(fakes.message, "Not sent");
}

static void backspace_removes_a_whole_character(void) {
    start("1");
    snprintf(app.ui.draft, sizeof(app.ui.draft), "a\xc5\xbc");
    messages__backspace(&app);
    CHECK_STR(app.ui.draft, "a");
}

static void new_message_to_a_contact(void) {
    fakes__set_file(STORE_CONTACTS_FILE, "600100200,Anna\n");
    start(NULL);
    fakes.choice = 1;
    messages__key(&app, BRUCE_INPUT_CODE_SELECT, 240, 320);
    CHECK_STR(fakes.choice_labels[0], "Type a number");
    CHECK_STR(fakes.choice_labels[1], "Anna");
    CHECK_EQ(app.ui.screen, UI_SCREEN_THREAD);
    CHECK_STR(app.ui.number, "600100200");
}

static void new_message_to_a_typed_number(void) {
    start(NULL);
    fakes.text_inputs[0] = "+48 600";
    messages__new(&app);
    CHECK_EQ(app.ui.screen, UI_SCREEN_LIST);
    CHECK_EQ(fakes.message_count, 1);
    fakes.text_input_next = 0;
    fakes.text_inputs[0] = "+48600100200";
    messages__new(&app);
    CHECK_STR(app.ui.number, "+48600100200");
}

static void a_later_start_opens_its_number(void) {
    start(NULL);
    messages__take_launch(&app);
    CHECK_EQ(app.ui.screen, UI_SCREEN_LIST);
    fakes.launch_number = "600100200";
    messages__take_launch(&app);
    CHECK_EQ(app.ui.screen, UI_SCREEN_THREAD);
    CHECK_STR(app.ui.number, "600100200");
}

static void menu_deletes_a_conversation(void) {
    fakes__set_file(STORE_MESSAGES_FILE, "5,i,r,1,a\n6,i,r,2,b\n");
    start(NULL);
    app.ui.selected = 1;
    /* "Delete conversation", then "Delete". */
    fakes.choices[0] = 0;
    fakes.choices[1] = 1;
    fakes.choice_queue = 2;
    messages__menu(&app);
    CHECK_EQ(store->message_count, 1);
    CHECK_STR(store->messages[0].number, "1");
    CHECK_EQ(app.ui.thread_count, 1);
}

SUITE(
    messages_tests, TEST(opening_moves_sim_messages_into_the_store), TEST(new_message_while_open_is_synced),
    TEST(opening_a_conversation_marks_it_read), TEST(typing_and_sending), TEST(failed_send_keeps_the_draft),
    TEST(backspace_removes_a_whole_character), TEST(new_message_to_a_contact),
    TEST(new_message_to_a_typed_number), TEST(a_later_start_opens_its_number),
    TEST(menu_deletes_a_conversation)
)
