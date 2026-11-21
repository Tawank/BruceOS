#include <stdlib.h>

#include "fakes.h"
#include "test.h"
#include "ui.h"

static ui_t ui;
static store_t *store;

static void start(void) {
    memset(&ui, 0, sizeof(ui));
    free(store);
    store = calloc(1, sizeof(store_t));
    ui.store = store;
    ui.status.state = BRUCE_CELLULAR_READY;
    ui.now_utc = fakes.now_utc;
    ui.utc_offset = 3600;
    ui__init_metrics();
    ui__set_origin(0, 0);
}

static void refresh(void) { ui.thread_count = store__threads(store, ui.threads, STORE_MESSAGES_MAX); }

static size_t wrap(const char *text, int max_chars, ui_line_t *lines) {
    return ui__wrap(text, max_chars, lines, 8);
}

static void wrap_breaks_at_spaces_and_line_breaks(void) {
    ui_line_t lines[8];
    CHECK_EQ(wrap("hello big world", 9, lines), 2);
    CHECK_EQ(lines[0].bytes, 9);
    CHECK(strncmp(lines[0].start, "hello big", 9) == 0);
    CHECK(strncmp(lines[1].start, "world", 5) == 0);
    CHECK_EQ(wrap("abcdefghij", 4, lines), 3);
    CHECK_EQ(lines[2].bytes, 2);
    CHECK_EQ(wrap("a\nb", 10, lines), 2);
    /* "żółw" is 4 characters in 7 bytes. */
    CHECK_EQ(wrap("\xc5\xbc\xc3\xb3\xc5\x82w", 4, lines), 1);
    CHECK_EQ(lines[0].bytes, 7);
}

static void draft_counts_ascii_and_ucs2(void) {
    char text[16];
    ui__draft_count("Hello", text, sizeof(text));
    CHECK_STR(text, "5/160");
    ui__draft_count("Cze\xc5\x9b\xc4\x87", text, sizeof(text));
    CHECK_STR(text, "5/70");
    char long_text[200];
    memset(long_text, 'a', 161);
    long_text[161] = '\0';
    CHECK(!ui__draft_fits(long_text));
    long_text[160] = '\0';
    CHECK(ui__draft_fits(long_text));
}

static void list_shows_names_previews_and_new_message(void) {
    start();
    store__parse_contacts(store, "600100200,Anna\n");
    store__add(store, fakes.now_utc - 600, true, true, "+48600100200", "See you at 6");
    store__add(store, fakes.now_utc - 300, false, false, "Orange", "STOP");
    refresh();
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_string("New message") != NULL);
    CHECK(fakes__find_icon("message-plus") != NULL);
    CHECK(fakes__find_string("Anna") != NULL);
    CHECK(fakes__find_string("See you at 6") != NULL);
    CHECK(fakes__find_string("You: STOP") != NULL);
    CHECK(fakes__find_string("05:35") != NULL);
    CHECK(ui__hit(&ui, 240, 320, 100, ui__row_rect(1, 240).y + 5).index == 1);
}

static void list_shows_the_modem_state(void) {
    start();
    ui.status.state = BRUCE_CELLULAR_NO_SIM;
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_string("No SIM card") != NULL);
    CHECK(fakes__find_string("No messages yet") != NULL);
}

static void thread_draws_bubbles_header_and_compose(void) {
    start();
    store__add(store, fakes.now_utc - 60, true, false, "600100200", "Are you coming?");
    store__add(store, fakes.now_utc, false, false, "600100200", "Yes");
    ui.screen = UI_SCREEN_THREAD;
    snprintf(ui.number, sizeof(ui.number), "600100200");
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_icon("arrow-left") != NULL);
    CHECK(fakes__find_string("600100200") != NULL);
    const fake_draw_t *in = fakes__find_string("Are you coming?");
    const fake_draw_t *out = fakes__find_string("Yes");
    CHECK(in != NULL && out != NULL && in->x < 120 && out->x > 120 && out->y > in->y);
    CHECK(fakes__find_string("Type a message") != NULL);
    snprintf(ui.draft, sizeof(ui.draft), "On my way");
    ui__draw(&ui, 240, 320);
    CHECK(fakes__find_string("On my way") != NULL);
    CHECK(fakes__find_string("9/160") != NULL);
}

static void thread_hits_back_compose_and_send(void) {
    start();
    ui.screen = UI_SCREEN_THREAD;
    CHECK_EQ(ui__hit(&ui, 240, 320, 10, 5).kind, UI_HIT_BACK);
    ui_rect_t send = ui__send_rect(240, 320);
    ui_rect_t box = ui__compose_rect(240, 320);
    CHECK_EQ(ui__hit(&ui, 240, 320, send.x + 2, send.y + 2).kind, UI_HIT_SEND);
    CHECK_EQ(ui__hit(&ui, 240, 320, box.x + 10, box.y + 2).kind, UI_HIT_COMPOSE);
    CHECK(box.x + box.w <= send.x && send.x + send.w <= 240 && send.y + send.h <= 320);
}

static void long_thread_scrolls(void) {
    start();
    snprintf(ui.number, sizeof(ui.number), "1");
    ui.screen = UI_SCREEN_THREAD;
    CHECK_EQ(ui__thread_max_scroll(&ui, 240, 320), 0);
    for (int i = 0; i < 20; ++i) store__add(store, i, true, false, "1", "message");
    CHECK(ui__thread_max_scroll(&ui, 240, 320) > 0);
}

SUITE(
    ui_tests, TEST(wrap_breaks_at_spaces_and_line_breaks), TEST(draft_counts_ascii_and_ucs2),
    TEST(list_shows_names_previews_and_new_message), TEST(list_shows_the_modem_state),
    TEST(thread_draws_bubbles_header_and_compose), TEST(thread_hits_back_compose_and_send),
    TEST(long_thread_scrolls)
)
