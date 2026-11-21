#include <stdlib.h>

#include "book.h"
#include "fakes.h"
#include "test.h"

static book_t book;

static void contacts_round_trip_sorted(void) {
    book__parse_contacts(&book, "+48600100200,Zoe\n600300400,adam, the plumber\r\nbroken line\n\n");
    CHECK_EQ(book.contact_count, 2);
    CHECK_STR(book.contacts[0].name, "adam, the plumber");
    CHECK_STR(book.contacts[0].number, "600300400");
    CHECK_STR(book.contacts[1].name, "Zoe");
    char text[256];
    book__format_contacts(&book, text, sizeof(text));
    CHECK_STR(text, "600300400,adam, the plumber\n+48600100200,Zoe\n");
}

static void contact_without_name_uses_number(void) {
    memset(&book, 0, sizeof(book));
    CHECK_EQ(book__add_contact(&book, "", "112"), 0);
    CHECK_STR(book.contacts[0].name, "112");
    CHECK_EQ(book__add_contact(&book, "x", ""), -1);
}

static void calls_round_trip(void) {
    book__parse_calls(&book, "1763700000,42,i,0,+48600100200\n1763600000,0,o,3,112\n");
    CHECK_EQ(book.call_count, 2);
    CHECK(book.calls[0].incoming);
    CHECK_EQ(book.calls[0].seconds, 42);
    CHECK_EQ(book.calls[1].result, BRUCE_CALL_RESULT_CANCELLED);
    char text[256];
    book__format_calls(&book, text, sizeof(text));
    CHECK_STR(text, "1763700000,42,i,0,+48600100200\n1763600000,0,o,3,112\n");
}

static void add_call_skips_duplicates_and_caps(void) {
    memset(&book, 0, sizeof(book));
    bruce_cellular_call_record_t record = {
        .incoming = true, .result = BRUCE_CALL_RESULT_MISSED, .started_utc = 5
    };
    snprintf(record.number, sizeof(record.number), "600100200");
    CHECK(book__add_call(&book, &record));
    CHECK(!book__add_call(&book, &record));
    for (int i = 0; i < BOOK_CALLS_MAX + 5; ++i) {
        record.started_utc = 100 + i;
        book__add_call(&book, &record);
    }
    CHECK_EQ(book.call_count, BOOK_CALLS_MAX);
    CHECK_EQ(book.calls[0].started_utc, 100 + BOOK_CALLS_MAX + 4);
}

static void numbers_match_on_last_digits(void) {
    CHECK(book__same_number("+48 600-100-200", "600100200"));
    CHECK(!book__same_number("600100201", "600100200"));
    CHECK(book__same_number("112", "112"));
    CHECK(!book__same_number("112", "1120"));
    memset(&book, 0, sizeof(book));
    book__add_contact(&book, "Mum", "+48600100200");
    CHECK_STR(book__name_for(&book, "600100200"), "Mum");
    CHECK(book__name_for(&book, "") == NULL);
}

static void ussd_codes(void) {
    CHECK(book__is_ussd("*100#"));
    CHECK(book__is_ussd("#123#"));
    CHECK(!book__is_ussd("100#"));
    CHECK(!book__is_ussd("*100"));
}

static void load_and_save_use_appdata(void) {
    fakes__set_file(BOOK_CONTACTS_FILE, "112,Emergency\n");
    fakes__set_file(BOOK_CALLS_FILE, "0,0,o,0,112\n");
    CHECK(book__load(&book));
    CHECK_EQ(book.contact_count, 1);
    CHECK_EQ(book.call_count, 1);
    book__add_contact(&book, "Anna", "123");
    CHECK(book__save_contacts(&book));
    CHECK_STR(fakes__file(BOOK_CONTACTS_FILE)->data, "123,Anna\n112,Emergency\n");
}

SUITE(
    book_tests, TEST(contacts_round_trip_sorted), TEST(contact_without_name_uses_number),
    TEST(calls_round_trip), TEST(add_call_skips_duplicates_and_caps), TEST(numbers_match_on_last_digits),
    TEST(ussd_codes), TEST(load_and_save_use_appdata)
)
