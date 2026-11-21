#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/cellular.h"

/* Shared with the phone app, which owns the contacts. */
#define STORE_DIR "/appdata/phone"
#define STORE_MESSAGES_FILE STORE_DIR "/messages.txt"
#define STORE_CONTACTS_FILE STORE_DIR "/contacts.txt"

#define STORE_MESSAGES_MAX 120
#define STORE_CONTACTS_MAX 100
#define STORE_NAME_MAX 32

typedef struct {
    int64_t utc; /* 0 when the time is unknown. */
    bool incoming;
    bool unread;
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    char text[BRUCE_CELLULAR_TEXT_MAX];
} store_message_t;

typedef struct {
    char name[STORE_NAME_MAX];
    char number[BRUCE_CELLULAR_NUMBER_MAX];
} store_contact_t;

/* A conversation: everything with one number. */
typedef struct {
    size_t last; /* Index of its newest message. */
    int unread;
} store_thread_t;

typedef struct {
    store_message_t messages[STORE_MESSAGES_MAX]; /* Oldest first. */
    size_t message_count;
    store_contact_t contacts[STORE_CONTACTS_MAX];
    size_t contact_count;
} store_t;

/* messages.txt: "utc,i|o,u|r,number,text" per line, text with \n and \\ escaped. */
void store__parse_messages(store_t *store, const char *text);
size_t store__format_messages(const store_t *store, char *out, size_t capacity);
/* contacts.txt: "number,name" per line, written by the phone app. */
void store__parse_contacts(store_t *store, const char *text);

bool store__load(store_t *store);
bool store__save(const store_t *store);

/* Appends a message, dropping the oldest when full. */
void store__add(
    store_t *store, int64_t utc, bool incoming, bool unread, const char *number, const char *text
);

/* Same number when the last 9 digits match, so "+48 600..." equals "600...". */
bool store__same_number(const char *a, const char *b);
const char *store__name_for(const store_t *store, const char *number);

/* Conversations, newest first. */
size_t store__threads(const store_t *store, store_thread_t *out, size_t capacity);
/* Indices of one conversation's messages, oldest first. */
size_t store__thread_messages(const store_t *store, const char *number, size_t *out, size_t capacity);
/* Returns whether any message was unread. */
bool store__mark_read(store_t *store, const char *number);
void store__delete_thread(store_t *store, const char *number);
