#include "store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

#define STORE_MATCH_DIGITS 9
/* A saved line never exceeds STORE_LINE_MAX, so a full store always fits STORE_FILE_MAX. */
#define STORE_LINE_MAX (BRUCE_CELLULAR_TEXT_MAX + 96)
#define STORE_FILE_MAX (STORE_MESSAGES_MAX * STORE_LINE_MAX)

static void store__copy(char *out, size_t capacity, const char *from, size_t length) {
    if (length >= capacity) length = capacity - 1;
    memcpy(out, from, length);
    out[length] = '\0';
}

/* Calls fn for each non-empty line, without its line ending. */
static void store__lines(const char *text, void (*fn)(store_t *, const char *), store_t *store) {
    static char line[STORE_LINE_MAX];
    while (text != NULL && *text != '\0') {
        const char *end = strchr(text, '\n');
        size_t length = end != NULL ? (size_t)(end - text) : strlen(text);
        store__copy(line, sizeof(line), text, length);
        size_t trimmed = strlen(line);
        if (trimmed > 0 && line[trimmed - 1] == '\r') line[trimmed - 1] = '\0';
        if (line[0] != '\0') fn(store, line);
        text = end != NULL ? end + 1 : NULL;
    }
}

static void store__clean_number(char *number) {
    for (char *c = number; *c != '\0'; ++c) {
        if (*c == ',' || *c == '\n' || *c == '\r') *c = ' ';
    }
}

void store__add(
    store_t *store, int64_t utc, bool incoming, bool unread, const char *number, const char *text
) {
    if (store->message_count == STORE_MESSAGES_MAX) {
        memmove(
            &store->messages[0], &store->messages[1], (STORE_MESSAGES_MAX - 1) * sizeof(store->messages[0])
        );
        store->message_count--;
    }
    store_message_t *message = &store->messages[store->message_count++];
    memset(message, 0, sizeof(*message));
    message->utc = utc;
    message->incoming = incoming;
    message->unread = unread;
    snprintf(message->number, sizeof(message->number), "%s", number);
    store__clean_number(message->number);
    snprintf(message->text, sizeof(message->text), "%s", text);
}

static void store__message_line(store_t *store, const char *line) {
    long long utc = 0;
    char direction = 0, read = 0;
    int consumed = 0;
    if (sscanf(line, "%lld,%c,%c,%n", &utc, &direction, &read, &consumed) < 3 || consumed == 0) return;
    const char *number = line + consumed;
    const char *comma = strchr(number, ',');
    if (comma == NULL) return;
    char copy[BRUCE_CELLULAR_NUMBER_MAX];
    store__copy(copy, sizeof(copy), number, (size_t)(comma - number));
    char text[BRUCE_CELLULAR_TEXT_MAX];
    size_t length = 0;
    for (const char *c = comma + 1; *c != '\0' && length + 1 < sizeof(text); ++c) {
        if (*c == '\\' && c[1] != '\0') {
            ++c;
            text[length++] = *c == 'n' ? '\n' : *c;
        } else {
            text[length++] = *c;
        }
    }
    text[length] = '\0';
    store__add(store, utc, direction == 'i', read == 'u', copy, text);
}

void store__parse_messages(store_t *store, const char *text) {
    store->message_count = 0;
    store__lines(text, store__message_line, store);
}

size_t store__format_messages(const store_t *store, char *out, size_t capacity) {
    size_t used = 0;
    if (capacity > 0) out[0] = '\0';
    for (size_t i = 0; i < store->message_count; ++i) {
        const store_message_t *message = &store->messages[i];
        char line[STORE_LINE_MAX];
        int length = snprintf(
            line,
            sizeof(line),
            "%lld,%c,%c,%s,",
            (long long)message->utc,
            message->incoming ? 'i' : 'o',
            message->unread ? 'u' : 'r',
            message->number
        );
        if (length < 0) break;
        size_t at = (size_t)length;
        for (const char *c = message->text; *c != '\0' && at + 3 < sizeof(line); ++c) {
            if (*c == '\r') continue;
            if (*c == '\n' || *c == '\\') line[at++] = '\\';
            line[at++] = *c == '\n' ? 'n' : *c;
        }
        line[at++] = '\n';
        if (used + at >= capacity) break;
        memcpy(out + used, line, at);
        used += at;
        out[used] = '\0';
    }
    return used;
}

static void store__contact_line(store_t *store, const char *line) {
    const char *comma = strchr(line, ',');
    if (comma == NULL || store->contact_count >= STORE_CONTACTS_MAX) return;
    store_contact_t *contact = &store->contacts[store->contact_count++];
    store__copy(contact->number, sizeof(contact->number), line, (size_t)(comma - line));
    snprintf(contact->name, sizeof(contact->name), "%s", comma + 1);
}

void store__parse_contacts(store_t *store, const char *text) {
    store->contact_count = 0;
    store__lines(text, store__contact_line, store);
}

/* Reads a whole file; a missing one reads as empty. */
static char *store__read(const char *path) {
    char *text = memory__malloc(STORE_FILE_MAX + 1);
    if (text == NULL) return NULL;
    size_t size = 0;
    bruce_file_id_t file;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        size_t got = 0;
        while (size < STORE_FILE_MAX &&
               storage__read(file, text + size, STORE_FILE_MAX - size, &got) == BRUCE_OK && got > 0) {
            size += got;
        }
        storage__close(file);
    }
    text[size] = '\0';
    return text;
}

bool store__load(store_t *store) {
    memset(store, 0, sizeof(*store));
    char *contacts = store__read(STORE_CONTACTS_FILE);
    if (contacts != NULL) store__parse_contacts(store, contacts);
    memory__free(contacts);
    char *messages = store__read(STORE_MESSAGES_FILE);
    if (messages != NULL) store__parse_messages(store, messages);
    memory__free(messages);
    return contacts != NULL && messages != NULL;
}

/* Writes a temporary file and renames it over the old one. */
bool store__save(const store_t *store) {
    char *text = memory__malloc(STORE_FILE_MAX);
    if (text == NULL) return false;
    size_t size = store__format_messages(store, text, STORE_FILE_MAX);
    (void)storage__mkdir(STORE_DIR);
    static const char temporary[] = STORE_MESSAGES_FILE ".tmp";
    bruce_file_id_t file;
    bool saved = storage__open(
                     temporary,
                     BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
                     &file
                 ) == BRUCE_OK;
    if (saved) {
        size_t written = 0;
        saved = storage__write(file, text, size, &written) == BRUCE_OK && written == size;
        saved = storage__close(file) == BRUCE_OK && saved;
    }
    memory__free(text);
    if (saved) {
        (void)storage__remove(STORE_MESSAGES_FILE);
        saved = storage__rename(temporary, STORE_MESSAGES_FILE) == BRUCE_OK;
    }
    return saved;
}

static size_t store__digits(const char *number, char *out, size_t capacity) {
    size_t count = 0;
    for (const char *c = number; *c != '\0' && count + 1 < capacity; ++c) {
        if (isdigit((unsigned char)*c)) out[count++] = *c;
    }
    out[count] = '\0';
    return count;
}

bool store__same_number(const char *a, const char *b) {
    char da[BRUCE_CELLULAR_NUMBER_MAX], db[BRUCE_CELLULAR_NUMBER_MAX];
    size_t la = store__digits(a, da, sizeof(da)), lb = store__digits(b, db, sizeof(db));
    /* Senders such as "Orange" have no digits. */
    if (la == 0 || lb == 0) return strcmp(a, b) == 0;
    if (la >= STORE_MATCH_DIGITS && lb >= STORE_MATCH_DIGITS)
        return strcmp(da + la - STORE_MATCH_DIGITS, db + lb - STORE_MATCH_DIGITS) == 0;
    return strcmp(da, db) == 0;
}

const char *store__name_for(const store_t *store, const char *number) {
    for (size_t i = 0; i < store->contact_count; ++i) {
        if (store__same_number(store->contacts[i].number, number)) return store->contacts[i].name;
    }
    return NULL;
}

size_t store__threads(const store_t *store, store_thread_t *out, size_t capacity) {
    size_t count = 0;
    /* Newest first: walk back, starting a conversation at its newest message. */
    for (size_t i = store->message_count; i-- > 0;) {
        const store_message_t *message = &store->messages[i];
        size_t t = 0;
        while (t < count && !store__same_number(store->messages[out[t].last].number, message->number)) t++;
        if (t == count) {
            if (count == capacity) continue;
            out[count++] = (store_thread_t){.last = i};
        }
        if (message->unread) out[t].unread++;
    }
    return count;
}

size_t store__thread_messages(const store_t *store, const char *number, size_t *out, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < store->message_count && count < capacity; ++i) {
        if (store__same_number(store->messages[i].number, number)) out[count++] = i;
    }
    return count;
}

bool store__mark_read(store_t *store, const char *number) {
    bool changed = false;
    for (size_t i = 0; i < store->message_count; ++i) {
        store_message_t *message = &store->messages[i];
        if (message->unread && store__same_number(message->number, number)) {
            message->unread = false;
            changed = true;
        }
    }
    return changed;
}

void store__delete_thread(store_t *store, const char *number) {
    char copy[BRUCE_CELLULAR_NUMBER_MAX];
    snprintf(copy, sizeof(copy), "%s", number);
    size_t kept = 0;
    for (size_t i = 0; i < store->message_count; ++i) {
        if (!store__same_number(store->messages[i].number, copy))
            store->messages[kept++] = store->messages[i];
    }
    store->message_count = kept;
}
