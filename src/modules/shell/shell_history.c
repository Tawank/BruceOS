#include "shell_history.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

#define SHELL_HISTORY_MAX_BYTES (64u * 1024u)
#define SHELL_HISTORY_OLD_PATH "/shell_history.old"

static bruce_result_t shell_history__size(bruce_file_id_t file, uint64_t *out_size) {
    return storage__seek(file, 0, SEEK_END, out_size);
}

static bruce_result_t shell_history__read_byte(bruce_file_id_t file, uint64_t offset, char *out) {
    uint64_t position = 0;
    bruce_result_t result = storage__seek(file, (int64_t)offset, SEEK_SET, &position);
    if (result != BRUCE_OK) return result;
    size_t size = 0;
    result = storage__read(file, out, 1, &size);
    return result == BRUCE_OK && size == 1 ? BRUCE_OK : BRUCE_ERR_IO;
}

static bruce_result_t shell_history__read_range(
    bruce_file_id_t file, uint64_t start, uint64_t end, char *line, size_t capacity
) {
    if (end < start || end - start >= capacity) return BRUCE_ERR_RESOURCE_LIMIT;
    uint64_t position = 0;
    bruce_result_t result = storage__seek(file, (int64_t)start, SEEK_SET, &position);
    if (result != BRUCE_OK) return result;
    size_t used = 0;
    size_t wanted = (size_t)(end - start);
    while (used < wanted) {
        size_t size = 0;
        result = storage__read(file, line + used, wanted - used, &size);
        if (result != BRUCE_OK || size == 0) return result == BRUCE_OK ? BRUCE_ERR_IO : result;
        used += size;
    }
    line[used] = '\0';
    return BRUCE_OK;
}

bruce_result_t shell_history__append(const char *path, const char *line) {
    if (path == NULL || line == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    size_t length = strlen(line);
    if (length == 0) return BRUCE_OK;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_APPEND, &file
    );
    if (result != BRUCE_OK) return result;
    uint64_t size = 0;
    result = shell_history__size(file, &size);
    (void)storage__close(file);
    if (result != BRUCE_OK) return result;

    if (size + length + 1 > SHELL_HISTORY_MAX_BYTES) {
        if (strcmp(path, SHELL_HISTORY_PATH) == 0) {
            (void)storage__remove(SHELL_HISTORY_OLD_PATH);
            (void)storage__rename(path, SHELL_HISTORY_OLD_PATH);
        } else {
            (void)storage__remove(path);
        }
    }

    result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_APPEND, &file
    );
    if (result != BRUCE_OK) return result;
    size_t offset = 0;
    while (offset < length) {
        size_t written = 0;
        result = storage__write(file, line + offset, length - offset, &written);
        if (result != BRUCE_OK || written == 0) break;
        offset += written;
    }
    if (result == BRUCE_OK && offset == length) {
        size_t written = 0;
        result = storage__write(file, "\n", 1, &written);
        if (result == BRUCE_OK && written != 1) result = BRUCE_ERR_IO;
    } else if (result == BRUCE_OK) {
        result = BRUCE_ERR_IO;
    }
    (void)storage__close(file);
    return result;
}

bruce_result_t shell_history__previous(
    const char *path, uint64_t before, char *line, size_t capacity, uint64_t *out_start
) {
    if (path == NULL || line == NULL || capacity == 0 || out_start == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    uint64_t size = 0;
    result = shell_history__size(file, &size);
    uint64_t pos = before < size ? before : size;
    char c = '\0';
    while (result == BRUCE_OK && pos > 0) {
        result = shell_history__read_byte(file, pos - 1, &c);
        if (result != BRUCE_OK || c != '\n') break;
        pos--;
    }
    uint64_t end = pos;
    while (result == BRUCE_OK && pos > 0) {
        result = shell_history__read_byte(file, pos - 1, &c);
        if (result != BRUCE_OK || c == '\n') break;
        pos--;
    }
    if (result == BRUCE_OK && end == pos) result = BRUCE_ERR_NOT_FOUND;
    if (result == BRUCE_OK) result = shell_history__read_range(file, pos, end, line, capacity);
    (void)storage__close(file);
    if (result == BRUCE_OK) *out_start = pos;
    return result;
}

bruce_result_t shell_history__next(
    const char *path, uint64_t current_start, char *line, size_t capacity, uint64_t *out_start
) {
    if (path == NULL || line == NULL || capacity == 0 || out_start == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    uint64_t size = 0;
    result = shell_history__size(file, &size);
    uint64_t pos = current_start;
    char c = '\0';
    while (result == BRUCE_OK && pos < size) {
        result = shell_history__read_byte(file, pos++, &c);
        if (result != BRUCE_OK || c == '\n') break;
    }
    while (result == BRUCE_OK && pos < size) {
        result = shell_history__read_byte(file, pos, &c);
        if (result != BRUCE_OK || c != '\n') break;
        pos++;
    }
    uint64_t start = pos;
    while (result == BRUCE_OK && pos < size) {
        result = shell_history__read_byte(file, pos, &c);
        if (result != BRUCE_OK || c == '\n') break;
        pos++;
    }
    if (result == BRUCE_OK && start == pos) result = BRUCE_ERR_NOT_FOUND;
    if (result == BRUCE_OK) result = shell_history__read_range(file, start, pos, line, capacity);
    (void)storage__close(file);
    if (result == BRUCE_OK) *out_start = start;
    return result;
}

void shell_history_browser__init(shell_history_browser_t *browser, char *draft, size_t capacity) {
    browser->draft = draft;
    browser->capacity = capacity;
    browser->offset = UINT64_MAX;
    browser->browsing = false;
    if (capacity > 0) draft[0] = '\0';
}

bool shell_history_browser__previous(shell_history_browser_t *browser, shell_line_editor_t *editor) {
    size_t capacity = browser->capacity < editor->capacity ? browser->capacity : editor->capacity;
    if (capacity == 0) return false;
    if (!browser->browsing) {
        size_t length = editor->length < capacity - 1 ? editor->length : capacity - 1;
        memcpy(browser->draft, editor->text, length);
        browser->draft[length] = '\0';
    }
    char *recalled = memory__malloc(capacity);
    if (recalled == NULL) return false;
    uint64_t start = 0;
    bruce_result_t result = shell_history__previous(
        SHELL_HISTORY_PATH, browser->browsing ? browser->offset : UINT64_MAX,
        recalled, capacity, &start
    );
    if (result != BRUCE_OK) {
        memory__free(recalled);
        return false;
    }
    shell_line_editor__set(editor, recalled);
    memory__free(recalled);
    browser->offset = start;
    browser->browsing = true;
    return true;
}

bool shell_history_browser__next(shell_history_browser_t *browser, shell_line_editor_t *editor) {
    if (!browser->browsing) return false;
    size_t capacity = browser->capacity < editor->capacity ? browser->capacity : editor->capacity;
    if (capacity == 0) return false;
    char *recalled = memory__malloc(capacity);
    if (recalled == NULL) return false;
    uint64_t start = 0;
    if (shell_history__next(SHELL_HISTORY_PATH, browser->offset, recalled, capacity, &start) == BRUCE_OK) {
        shell_line_editor__set(editor, recalled);
        browser->offset = start;
    } else {
        shell_line_editor__set(editor, browser->draft);
        browser->browsing = false;
    }
    memory__free(recalled);
    return true;
}

void shell_history_browser__reset(shell_history_browser_t *browser) {
    browser->offset = UINT64_MAX;
    browser->browsing = false;
}
