#include "book.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/appdata.h"
#include "core_sdk/memory.h"

#define BOOK_LINE_MAX 96
#define BOOK_MATCH_DIGITS 9
#define BOOK_DEDUP_CALLS 16

static void book__copy(char *out, size_t capacity, const char *from, size_t length) {
    if (length >= capacity) length = capacity - 1;
    memcpy(out, from, length);
    out[length] = '\0';
}

/* Calls fn for each line of text, with the line ending removed. */
static void book__lines(const char *text, void (*fn)(book_t *, const char *), book_t *book) {
    while (text != NULL && *text != '\0') {
        const char *end = strchr(text, '\n');
        size_t length = end != NULL ? (size_t)(end - text) : strlen(text);
        char line[BOOK_LINE_MAX];
        book__copy(line, sizeof(line), text, length);
        size_t trimmed = strlen(line);
        if (trimmed > 0 && line[trimmed - 1] == '\r') line[trimmed - 1] = '\0';
        if (line[0] != '\0') fn(book, line);
        text = end != NULL ? end + 1 : NULL;
    }
}

static void book__clean(char *text) {
    for (char *c = text; *c != '\0'; ++c) {
        if (*c == '\n' || *c == '\r') *c = ' ';
    }
}

static int book__compare_names(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || ca == 0) return ca - cb;
    }
}

int book__add_contact(book_t *book, const char *name, const char *number) {
    if (book->contact_count >= BOOK_CONTACTS_MAX || number == NULL || number[0] == '\0') return -1;
    book_contact_t contact = {0};
    snprintf(contact.name, sizeof(contact.name), "%s", name != NULL && name[0] != '\0' ? name : number);
    snprintf(contact.number, sizeof(contact.number), "%s", number);
    book__clean(contact.name);
    for (char *c = contact.number; *c != '\0'; ++c) {
        if (*c == ',' || *c == '\n') *c = ' ';
    }
    size_t index = 0;
    while (index < book->contact_count &&
           book__compare_names(book->contacts[index].name, contact.name) <= 0) {
        index++;
    }
    memmove(
        &book->contacts[index + 1], &book->contacts[index], (book->contact_count - index) * sizeof(contact)
    );
    book->contacts[index] = contact;
    book->contact_count++;
    return (int)index;
}

void book__remove_contact(book_t *book, size_t index) {
    if (index >= book->contact_count) return;
    memmove(
        &book->contacts[index],
        &book->contacts[index + 1],
        (book->contact_count - index - 1) * sizeof(book->contacts[0])
    );
    book->contact_count--;
}

static void book__contact_line(book_t *book, const char *line) {
    const char *comma = strchr(line, ',');
    if (comma == NULL) return;
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    book__copy(number, sizeof(number), line, (size_t)(comma - line));
    book__add_contact(book, comma + 1, number);
}

void book__parse_contacts(book_t *book, const char *text) {
    book->contact_count = 0;
    book__lines(text, book__contact_line, book);
}

size_t book__format_contacts(const book_t *book, char *out, size_t capacity) {
    size_t used = 0;
    if (capacity > 0) out[0] = '\0';
    for (size_t i = 0; i < book->contact_count; ++i) {
        int written = snprintf(
            out + used, capacity - used, "%s,%s\n", book->contacts[i].number, book->contacts[i].name
        );
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
    return used;
}

static void book__call_line(book_t *book, const char *line) {
    if (book->call_count >= BOOK_CALLS_MAX) return;
    long long utc = 0;
    unsigned long seconds = 0;
    char direction = 0;
    int result = 0;
    int consumed = 0;
    if (sscanf(line, "%lld,%lu,%c,%d,%n", &utc, &seconds, &direction, &result, &consumed) < 4 ||
        consumed == 0) {
        return;
    }
    book_call_t *call = &book->calls[book->call_count++];
    memset(call, 0, sizeof(*call));
    call->started_utc = utc;
    call->seconds = (uint32_t)seconds;
    call->incoming = direction == 'i';
    call->result = (bruce_call_result_t)result;
    snprintf(call->number, sizeof(call->number), "%s", line + consumed);
}

void book__parse_calls(book_t *book, const char *text) {
    book->call_count = 0;
    book__lines(text, book__call_line, book);
}

size_t book__format_calls(const book_t *book, char *out, size_t capacity) {
    size_t used = 0;
    if (capacity > 0) out[0] = '\0';
    for (size_t i = 0; i < book->call_count; ++i) {
        const book_call_t *call = &book->calls[i];
        int written = snprintf(
            out + used,
            capacity - used,
            "%lld,%lu,%c,%d,%s\n",
            (long long)call->started_utc,
            (unsigned long)call->seconds,
            call->incoming ? 'i' : 'o',
            (int)call->result,
            call->number
        );
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
    return used;
}

static char *book__read(const char *name) {
    size_t capacity = BOOK_CONTACTS_MAX * BOOK_LINE_MAX;
    char *text = memory__malloc(capacity + 1);
    if (text == NULL) return NULL;
    size_t size = 0;
    bruce_result_t result = appdata__read(name, text, capacity, &size);
    if (result != BRUCE_OK && result != BRUCE_ERR_RESOURCE_LIMIT) size = 0;
    text[size] = '\0';
    return text;
}

bool book__load(book_t *book) {
    memset(book, 0, sizeof(*book));
    char *contacts = book__read(BOOK_CONTACTS_FILE);
    char *calls = book__read(BOOK_CALLS_FILE);
    if (contacts != NULL) book__parse_contacts(book, contacts);
    if (calls != NULL) book__parse_calls(book, calls);
    bool ok = contacts != NULL && calls != NULL;
    memory__free(contacts);
    memory__free(calls);
    return ok;
}

static bool
book__save(const char *name, size_t (*format)(const book_t *, char *, size_t), const book_t *book) {
    size_t capacity = BOOK_CONTACTS_MAX * BOOK_LINE_MAX;
    char *text = memory__malloc(capacity);
    if (text == NULL) return false;
    size_t size = format(book, text, capacity);
    bool saved = appdata__write(name, text, size) == BRUCE_OK;
    memory__free(text);
    return saved;
}

bool book__save_contacts(const book_t *book) {
    return book__save(BOOK_CONTACTS_FILE, book__format_contacts, book);
}

bool book__save_calls(const book_t *book) { return book__save(BOOK_CALLS_FILE, book__format_calls, book); }

bool book__add_call(book_t *book, const bruce_cellular_call_record_t *record) {
    for (size_t i = 0; i < book->call_count && i < BOOK_DEDUP_CALLS; ++i) {
        const book_call_t *call = &book->calls[i];
        if (call->started_utc == record->started_utc && call->seconds == record->seconds &&
            call->incoming == record->incoming && call->result == record->result &&
            strcmp(call->number, record->number) == 0) {
            return false;
        }
    }
    size_t keep = book->call_count < BOOK_CALLS_MAX ? book->call_count : BOOK_CALLS_MAX - 1;
    memmove(&book->calls[1], &book->calls[0], keep * sizeof(book->calls[0]));
    book_call_t *call = &book->calls[0];
    memset(call, 0, sizeof(*call));
    snprintf(call->number, sizeof(call->number), "%s", record->number);
    book__clean(call->number);
    call->started_utc = record->started_utc;
    call->seconds = record->seconds;
    call->incoming = record->incoming;
    call->result = record->result;
    book->call_count = keep + 1;
    return true;
}

void book__remove_call(book_t *book, size_t index) {
    if (index >= book->call_count) return;
    memmove(
        &book->calls[index], &book->calls[index + 1], (book->call_count - index - 1) * sizeof(book->calls[0])
    );
    book->call_count--;
}

static size_t book__digits(const char *number, char *out, size_t capacity) {
    size_t count = 0;
    for (const char *c = number; *c != '\0' && count + 1 < capacity; ++c) {
        if (isdigit((unsigned char)*c)) out[count++] = *c;
    }
    out[count] = '\0';
    return count;
}

bool book__same_number(const char *a, const char *b) {
    char da[BRUCE_CELLULAR_NUMBER_MAX], db[BRUCE_CELLULAR_NUMBER_MAX];
    size_t la = book__digits(a, da, sizeof(da)), lb = book__digits(b, db, sizeof(db));
    if (la == 0 || lb == 0) return false;
    if (la >= BOOK_MATCH_DIGITS && lb >= BOOK_MATCH_DIGITS) {
        return strcmp(da + la - BOOK_MATCH_DIGITS, db + lb - BOOK_MATCH_DIGITS) == 0;
    }
    return strcmp(da, db) == 0;
}

const char *book__name_for(const book_t *book, const char *number) {
    if (number == NULL || number[0] == '\0') return NULL;
    for (size_t i = 0; i < book->contact_count; ++i) {
        if (book__same_number(book->contacts[i].number, number)) return book->contacts[i].name;
    }
    return NULL;
}

bool book__is_ussd(const char *number) {
    size_t length = strlen(number);
    return length >= 2 && (number[0] == '*' || number[0] == '#') && number[length - 1] == '#';
}
