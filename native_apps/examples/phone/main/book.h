#pragma once

/* Contacts and call history, kept as text files in the app's appdata folder. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/cellular.h"

#define BOOK_NAME_MAX 32
#define BOOK_CONTACTS_MAX 100
#define BOOK_CALLS_MAX 100
#define BOOK_CONTACTS_FILE "contacts.txt"
#define BOOK_CALLS_FILE "history.txt"

typedef struct {
    char name[BOOK_NAME_MAX];
    char number[BRUCE_CELLULAR_NUMBER_MAX];
} book_contact_t;

typedef struct {
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    int64_t started_utc;
    uint32_t seconds;
    bool incoming;
    bruce_call_result_t result;
} book_call_t;

typedef struct {
    book_contact_t contacts[BOOK_CONTACTS_MAX]; /* Sorted by name. */
    size_t contact_count;
    book_call_t calls[BOOK_CALLS_MAX]; /* Newest first. */
    size_t call_count;
} book_t;

/* "number,name" per line. */
void book__parse_contacts(book_t *book, const char *text);
size_t book__format_contacts(const book_t *book, char *out, size_t capacity);

/* "utc,seconds,i|o,result,number" per line, newest first. */
void book__parse_calls(book_t *book, const char *text);
size_t book__format_calls(const book_t *book, char *out, size_t capacity);

bool book__load(book_t *book);
bool book__save_contacts(const book_t *book);
bool book__save_calls(const book_t *book);

/* Adds a call from the modem's log unless the newest calls already hold it. */
bool book__add_call(book_t *book, const bruce_cellular_call_record_t *record);
void book__remove_call(book_t *book, size_t index);

/* Returns the index the contact landed at, or -1 when the book is full. */
int book__add_contact(book_t *book, const char *name, const char *number);
void book__remove_contact(book_t *book, size_t index);

/* Same number when the digits match, comparing the last 9 so +48 123... and 123... agree. */
bool book__same_number(const char *a, const char *b);
const char *book__name_for(const book_t *book, const char *number);

/* USSD codes such as *100# and #123#. */
bool book__is_ussd(const char *number);
