#include <stdlib.h>

#include "fakes.h"
#include "store.h"
#include "test.h"

static store_t *new_store(void) { return calloc(1, sizeof(store_t)); }

static void roundtrip_keeps_line_breaks_and_backslashes(void) {
    store_t *store = new_store();
    store__add(store, 1000, true, true, "+48600100200", "Hi\nsee C:\\temp");
    store__add(store, 0, false, false, "Orange", "plain");
    char text[2048];
    store__format_messages(store, text, sizeof(text));
    CHECK_STR(text, "1000,i,u,+48600100200,Hi\\nsee C:\\\\temp\n0,o,r,Orange,plain\n");
    store_t *loaded = new_store();
    store__parse_messages(loaded, text);
    CHECK_EQ(loaded->message_count, 2);
    CHECK_STR(loaded->messages[0].text, "Hi\nsee C:\\temp");
    CHECK(loaded->messages[0].incoming && loaded->messages[0].unread);
    CHECK(!loaded->messages[1].incoming && !loaded->messages[1].unread);
    CHECK_STR(loaded->messages[1].number, "Orange");
    free(store);
    free(loaded);
}

static void full_store_drops_the_oldest(void) {
    store_t *store = new_store();
    for (int i = 0; i < STORE_MESSAGES_MAX + 2; ++i) {
        char text[16];
        snprintf(text, sizeof(text), "m%d", i);
        store__add(store, i, true, false, "1", text);
    }
    CHECK_EQ(store->message_count, STORE_MESSAGES_MAX);
    CHECK_STR(store->messages[0].text, "m2");
    free(store);
}

static void threads_group_by_number_newest_first(void) {
    store_t *store = new_store();
    store__parse_contacts(store, "600100200,Anna\n");
    store__add(store, 1, true, false, "+48600100200", "a1");
    store__add(store, 2, true, true, "Orange", "o1");
    store__add(store, 3, false, false, "600100200", "a2");
    store__add(store, 4, true, true, "48600100200", "a3");
    store_thread_t threads[8];
    size_t count = store__threads(store, threads, 8);
    CHECK_EQ(count, 2);
    CHECK_STR(store->messages[threads[0].last].text, "a3");
    CHECK_EQ(threads[0].unread, 1);
    CHECK_STR(store->messages[threads[1].last].text, "o1");
    CHECK_STR(store__name_for(store, "+48 600 100 200"), "Anna");
    CHECK(store__name_for(store, "Orange") == NULL);

    size_t indices[8];
    CHECK_EQ(store__thread_messages(store, "600100200", indices, 8), 3);
    CHECK(store__mark_read(store, "600100200"));
    CHECK(!store__mark_read(store, "600100200"));
    store__delete_thread(store, "+48600100200");
    CHECK_EQ(store->message_count, 1);
    CHECK_STR(store->messages[0].text, "o1");
    free(store);
}

static void load_and_save_use_the_phone_folder(void) {
    fakes__set_file(STORE_CONTACTS_FILE, "600100200,Anna\n");
    fakes__set_file(STORE_MESSAGES_FILE, "5,i,u,600100200,hello\n");
    store_t *store = new_store();
    CHECK(store__load(store));
    CHECK_EQ(store->contact_count, 1);
    CHECK_EQ(store->message_count, 1);
    store__add(store, 6, false, false, "600100200", "hi");
    CHECK(store__save(store));
    CHECK_STR(fakes__file(STORE_MESSAGES_FILE)->data, "5,i,u,600100200,hello\n6,o,r,600100200,hi\n");
    CHECK(fakes__file(STORE_MESSAGES_FILE ".tmp") == NULL);
    free(store);
}

SUITE(
    store_tests, TEST(roundtrip_keeps_line_breaks_and_backslashes), TEST(full_store_drops_the_oldest),
    TEST(threads_group_by_number_newest_first), TEST(load_and_save_use_the_phone_folder)
)
