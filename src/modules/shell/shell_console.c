#include "shell_console.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"
#include "core_sdk/stdio.h"
#include "modules/bnu/bnu_app.h"
#include "shell_builtins.h"
#include "shell_history.h"
#include "shell_internal.h"
#include "shell_line_editor.h"

#define SHELL_CONSOLE_ESCAPE_MAX 8
#define SHELL_CONSOLE_ESCAPE_TIMEOUT_MS 50

#define SHELL_CONSOLE_CTRL_A 0x01
#define SHELL_CONSOLE_CTRL_E 0x05
#define SHELL_CONSOLE_CTRL_U 0x15
#define SHELL_CONSOLE_ESCAPE 0x1b
#define SHELL_CONSOLE_DELETE 0x7f

static const char SHELL_CONSOLE_PROMPT[] = "\r\033[2K\033[1;36mbruce\033[0m$ ";
static volatile bool s_shell_console_ready;

typedef struct {
    char name[BRUCE_STORAGE_NAME_MAX];
    bool directory;
} shell_console_match_t;

typedef struct {
    shell_console_match_t *items;
    size_t count;
} shell_console_matches_t;

typedef struct {
    size_t token_start;
    bool first_token;
    char *decoded;
    size_t decoded_capacity;
} shell_console_token_t;

typedef struct {
    bool pending;
    size_t cursor;
    char *line;
    size_t line_capacity;
} shell_console_tab_state_t;

static void shell_console__token_free(shell_console_token_t *token) {
    if (token == NULL) return;
    memory__free(token->decoded);
    token->decoded = NULL;
    token->decoded_capacity = 0;
}

static bool shell_console__token_reserve(shell_console_token_t *token, size_t needed) {
    if (needed <= token->decoded_capacity) return true;
    size_t capacity = token->decoded_capacity == 0 ? 32u : token->decoded_capacity;
    while (capacity < needed) capacity *= 2u;
    if (capacity > SHELL__LINE_MAX) capacity = SHELL__LINE_MAX;
    if (needed > capacity) return false;
    char *grown = memory__realloc(token->decoded, capacity);
    if (grown == NULL) return false;
    token->decoded = grown;
    token->decoded_capacity = capacity;
    return true;
}

static void shell_console__tab_state_free(shell_console_tab_state_t *tab_state) {
    if (tab_state == NULL) return;
    memory__free(tab_state->line);
    tab_state->line = NULL;
    tab_state->line_capacity = 0;
    tab_state->pending = false;
    tab_state->cursor = 0;
}

static bool shell_console__tab_state_remember_capacity(shell_console_tab_state_t *tab_state, size_t needed) {
    if (needed <= tab_state->line_capacity) return true;
    size_t capacity = tab_state->line_capacity == 0 ? 32u : tab_state->line_capacity;
    while (capacity < needed) capacity *= 2u;
    if (capacity > SHELL__LINE_MAX) capacity = SHELL__LINE_MAX;
    if (needed > capacity) return false;
    char *grown = memory__realloc(tab_state->line, capacity);
    if (grown == NULL) return false;
    tab_state->line = grown;
    tab_state->line_capacity = capacity;
    return true;
}

static void shell_console__redraw(const shell_line_editor_t *editor) {
    (void)stdio__write(SHELL_CONSOLE_PROMPT, sizeof(SHELL_CONSOLE_PROMPT) - 1);
    (void)stdio__write(editor->text, editor->length);
    if (editor->cursor < editor->length) {
        stdio__printf("\033[%uD", (unsigned)(editor->length - editor->cursor));
    }
}

static int shell_console__read_byte(uint32_t timeout_ms) {
    unsigned char byte;
    size_t size = 0;
    bruce_result_t result = stdio__read(&byte, 1, timeout_ms, &size);
    return result == BRUCE_OK && size == 1 ? byte : result;
}

static size_t shell_console__common_prefix(const char *a, const char *b) {
    size_t length = 0;
    while (a[length] != '\0' && b[length] != '\0' && a[length] == b[length]) length++;
    return length;
}

static void shell_console__matches_free(shell_console_matches_t *matches) {
    memory__free(matches->items);
    matches->items = NULL;
    matches->count = 0;
}

static bool shell_console__matches_add(shell_console_matches_t *matches, const char *name, bool directory) {
    size_t length = strlen(name);
    if (length >= BRUCE_STORAGE_NAME_MAX) return false;
    shell_console_match_t *grown = memory__realloc(matches->items, (matches->count + 1) * sizeof(*grown));
    if (grown == NULL) return false;
    matches->items = grown;
    memcpy(matches->items[matches->count].name, name, length + 1);
    matches->items[matches->count].directory = directory;
    matches->count++;
    return true;
}

static size_t shell_console__matches_common_length(const shell_console_matches_t *matches) {
    if (matches->count == 0) return 0;
    size_t common = strlen(matches->items[0].name);
    for (size_t i = 1; i < matches->count; ++i) {
        size_t shared = shell_console__common_prefix(matches->items[0].name, matches->items[i].name);
        if (shared < common) common = shared;
    }
    return common;
}

static bool shell_console__resolve_path(const char *path, char *out_path) {
    char combined[BRUCE_STORAGE_PATH_MAX * 2];
    const char *working_directory = bnu__get_working_directory();
    if (path == NULL || path[0] == '\0') path = working_directory;
    int written = path[0] == '/' ? snprintf(combined, sizeof(combined), "%s", path)
                                 : snprintf(
                                       combined,
                                       sizeof(combined),
                                       "%s%s%s",
                                       working_directory,
                                       strcmp(working_directory, "/") == 0 ? "" : "/",
                                       path
                                   );
    if (written < 0 || (size_t)written >= sizeof(combined)) return false;

    size_t out_length = 1;
    out_path[0] = '/';
    out_path[1] = '\0';
    const char *cursor = combined;
    while (*cursor != '\0') {
        while (*cursor == '/') cursor++;
        const char *component = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - component);
        if (length == 0 || (length == 1 && component[0] == '.')) continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            while (out_length > 1 && out_path[out_length - 1] != '/') out_length--;
            if (out_length > 1) out_length--;
            out_path[out_length] = '\0';
            continue;
        }
        size_t separator = out_length > 1 ? 1u : 0u;
        if (out_length + separator + length >= BRUCE_STORAGE_PATH_MAX) return false;
        if (separator != 0) out_path[out_length++] = '/';
        memcpy(out_path + out_length, component, length);
        out_length += length;
        out_path[out_length] = '\0';
    }
    return true;
}

static bool shell_console__tokenize(const shell_line_editor_t *editor, shell_console_token_t *token) {
    shell_console__token_free(token);
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    bool saw_token = false;
    size_t token_start = 0;
    size_t decoded_length = 0;
    bool in_token = false;

    for (size_t i = 0; i < editor->cursor; ++i) {
        char c = editor->text[i];
        if (escaped) {
            if (!in_token) {
                in_token = true;
                token_start = i - 1;
                decoded_length = 0;
            }
            if (!shell_console__token_reserve(token, decoded_length + 2)) return false;
            token->decoded[decoded_length++] = c;
            escaped = false;
            continue;
        }
        if (!single && c == '\\') {
            if (!in_token) {
                in_token = true;
                token_start = i;
                decoded_length = 0;
            }
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            if (!in_token) {
                in_token = true;
                token_start = i;
                decoded_length = 0;
            }
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            if (!in_token) {
                in_token = true;
                token_start = i;
                decoded_length = 0;
            }
            double_quote = !double_quote;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (single || double_quote) {
                if (!shell_console__token_reserve(token, decoded_length + 2)) return false;
                token->decoded[decoded_length++] = c;
                continue;
            }
            if (!in_token) continue;
            saw_token = true;
            in_token = false;
            decoded_length = 0;
            continue;
        }
        if (!in_token) {
            in_token = true;
            token_start = i;
            decoded_length = 0;
        }
        if (!shell_console__token_reserve(token, decoded_length + 2)) return false;
        token->decoded[decoded_length++] = c;
    }

    if (escaped || single || double_quote || !in_token || decoded_length == 0) return false;
    token->decoded[decoded_length] = '\0';
    token->token_start = token_start;
    token->first_token = !saw_token;
    return true;
}

static bool shell_console__encode_token(const char *text, char *out, size_t capacity) {
    size_t used = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        bool escape = *p == ' ' || *p == '\t' || *p == '\\' || *p == '\'' || *p == '"';
        if (escape) {
            if (used + 2 > capacity) return false;
            out[used++] = '\\';
        } else if (used + 1 >= capacity) {
            return false;
        }
        out[used++] = *p;
    }
    if (used >= capacity) return false;
    out[used] = '\0';
    return true;
}

static bool shell_console__collect_command_matches(const char *prefix, shell_console_matches_t *matches) {
    size_t prefix_length = strlen(prefix);
    for (size_t i = 0; i < shell_builtins__count(); ++i) {
        const char *candidate = shell_builtins__name(i);
        if (candidate != NULL && strncmp(candidate, prefix, prefix_length) == 0 &&
            !shell_console__matches_add(matches, candidate, false)) {
            return false;
        }
    }
    size_t app_count = app_runner__command_count();
    for (size_t i = 0; i < app_count; ++i) {
        const char *candidate = app_runner__command_name(i);
        if (candidate != NULL && strncmp(candidate, prefix, prefix_length) == 0 &&
            !shell_console__matches_add(matches, candidate, false)) {
            return false;
        }
    }
    return true;
}

static bool shell_console__collect_file_matches(
    const char *token, shell_console_matches_t *matches, char *prefix_out, size_t prefix_capacity
) {
    const char *slash = strrchr(token, '/');
    char directory[BRUCE_STORAGE_PATH_MAX];
    char resolved[BRUCE_STORAGE_PATH_MAX];
    const char *name_prefix = token;
    prefix_out[0] = '\0';

    if (slash != NULL) {
        size_t prefix_length = (size_t)(slash - token + 1);
        if (prefix_length >= prefix_capacity || prefix_length >= sizeof(directory)) return false;
        memcpy(prefix_out, token, prefix_length);
        prefix_out[prefix_length] = '\0';
        memcpy(directory, token, prefix_length);
        directory[prefix_length] = '\0';
        name_prefix = slash + 1;
    } else {
        memcpy(directory, bnu__get_working_directory(), strlen(bnu__get_working_directory()) + 1);
    }

    if (slash != NULL) {
        if (directory[0] == '\0') memcpy(directory, "/", 2);
        if (!shell_console__resolve_path(directory, resolved)) return false;
    } else if (!shell_console__resolve_path(NULL, resolved)) {
        return false;
    }

    size_t count = 0;
    bruce_result_t result = storage__list(resolved, NULL, 0, &count);
    if (result != BRUCE_OK || count == 0) return result == BRUCE_OK;
    bruce_storage_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return false;
    result = storage__list(resolved, entries, count, &count);
    if (result == BRUCE_OK) {
        size_t prefix_length = strlen(name_prefix);
        for (size_t i = 0; i < count; ++i) {
            if (strncmp(entries[i].name, name_prefix, prefix_length) == 0 &&
                !shell_console__matches_add(matches, entries[i].name, entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY)) {
                result = BRUCE_ERR_NO_MEMORY;
                break;
            }
        }
    }
    memory__free(entries);
    return result == BRUCE_OK;
}

static void shell_console__print_matches(const shell_console_matches_t *matches) {
    (void)stdio__write("\r\n", 2);
    for (size_t i = 0; i < matches->count; ++i) {
        stdio__printf("%s%s\r\n", matches->items[i].name, matches->items[i].directory ? "/" : "");
    }
}

static bool shell_console__replace_token(
    shell_line_editor_t *editor, size_t token_start, const char *replacement, bool append_space
) {
    char encoded[SHELL__LINE_MAX];
    char line[SHELL__LINE_MAX];
    if (!shell_console__encode_token(replacement, encoded, sizeof(encoded))) return false;
    size_t used = token_start;
    size_t encoded_length = strlen(encoded);
    if (used + encoded_length + (append_space ? 1u : 0u) >= sizeof(line)) return false;
    if (used > 0) memcpy(line, editor->text, used);
    memcpy(line + used, encoded, encoded_length);
    used += encoded_length;
    if (append_space) line[used++] = ' ';
    line[used] = '\0';
    if (strcmp(line, editor->text) == 0) return false;
    shell_line_editor__set(editor, line);
    return true;
}

static bool shell_console__tab_is_repeated(
    const shell_console_tab_state_t *tab_state, const shell_line_editor_t *editor
) {
    return tab_state->pending && tab_state->line != NULL && tab_state->cursor == editor->cursor &&
           strcmp(tab_state->line, editor->text) == 0;
}

static void shell_console__tab_remember(shell_console_tab_state_t *tab_state, const shell_line_editor_t *editor) {
    if (!shell_console__tab_state_remember_capacity(tab_state, editor->length + 1)) {
        tab_state->pending = false;
        return;
    }
    tab_state->pending = true;
    tab_state->cursor = editor->cursor;
    memcpy(tab_state->line, editor->text, editor->length + 1);
}

static void shell_console__tab_reset(shell_console_tab_state_t *tab_state) { tab_state->pending = false; }

static bool shell_console__complete(shell_line_editor_t *editor, shell_console_tab_state_t *tab_state) {
    if (editor->cursor != editor->length || editor->cursor == 0) return false;

    shell_console_token_t token = {0};
    if (!shell_console__tokenize(editor, &token)) {
        shell_console__token_free(&token);
        return false;
    }

    shell_console_matches_t matches = {0};
    char *file_prefix = memory__calloc(SHELL__LINE_MAX, 1u);
    if (file_prefix == NULL) {
        shell_console__token_free(&token);
        return false;
    }
    bool use_files = !token.first_token || strchr(token.decoded, '/') != NULL;
    bool ok = use_files ? shell_console__collect_file_matches(token.decoded, &matches, file_prefix, SHELL__LINE_MAX)
                        : shell_console__collect_command_matches(token.decoded, &matches);
    if (!ok) {
        memory__free(file_prefix);
        shell_console__matches_free(&matches);
        shell_console__token_free(&token);
        return false;
    }
    if (matches.count == 0 && token.first_token && strchr(token.decoded, '/') == NULL) {
        ok = shell_console__collect_file_matches(token.decoded, &matches, file_prefix, SHELL__LINE_MAX);
        use_files = true;
    }
    if (!ok || matches.count == 0) {
        memory__free(file_prefix);
        shell_console__matches_free(&matches);
        shell_console__token_free(&token);
        return false;
    }

    size_t typed_length = strlen(use_files ? strrchr(token.decoded, '/') != NULL ? strrchr(token.decoded, '/') + 1 : token.decoded
                                           : token.decoded);
    size_t common_length = shell_console__matches_common_length(&matches);
    bool repeated = shell_console__tab_is_repeated(tab_state, editor);
    bool changed = false;

    if (matches.count == 1 || common_length > typed_length) {
        char *replacement = memory__malloc(SHELL__LINE_MAX);
        size_t prefix_length = strlen(use_files ? file_prefix : "");
        if (replacement != NULL && prefix_length + common_length + 2 < SHELL__LINE_MAX) {
            if (prefix_length > 0) memcpy(replacement, file_prefix, prefix_length);
            memcpy(replacement + prefix_length, matches.items[0].name, common_length);
            prefix_length += common_length;
            replacement[prefix_length] = '\0';
            if (matches.count == 1 && matches.items[0].directory) {
                replacement[prefix_length++] = '/';
                replacement[prefix_length] = '\0';
            }
            changed = shell_console__replace_token(
                editor,
                token.token_start,
                replacement,
                matches.count == 1 && !matches.items[0].directory
            );
        }
        memory__free(replacement);
    }

    if (!changed && repeated) shell_console__print_matches(&matches);
    if (changed || matches.count > 1) shell_console__tab_remember(tab_state, editor);
    else shell_console__tab_reset(tab_state);
    bool redraw = changed || (!changed && repeated);
    memory__free(file_prefix);
    shell_console__matches_free(&matches);
    shell_console__token_free(&token);
    return redraw;
}

static size_t shell_console__read_escape(unsigned char *sequence, size_t capacity) {
    size_t used = 0;
    while (used < capacity) {
        int input = shell_console__read_byte(SHELL_CONSOLE_ESCAPE_TIMEOUT_MS);
        if (input < 0) break;
        unsigned char byte = (unsigned char)input;
        sequence[used++] = byte;
        if (used > 1 && byte >= 0x40 && byte <= 0x7e) break;
    }
    return used;
}

static bool shell_console__handle_escape(
    shell_line_editor_t *editor, shell_history_browser_t *history
) {
    unsigned char sequence[SHELL_CONSOLE_ESCAPE_MAX] = {0};
    size_t size = shell_console__read_escape(sequence, sizeof(sequence));
    if (size < 2 || sequence[0] != '[') return false;

    switch (sequence[size - 1]) {
        case 'A': return shell_history_browser__previous(history, editor);
        case 'B': return shell_history_browser__next(history, editor);
        case 'C': return shell_line_editor__right(editor);
        case 'D': return shell_line_editor__left(editor);
        case 'H':
            if (editor->cursor == 0) return false;
            editor->cursor = 0;
            return true;
        case 'F':
            if (editor->cursor == editor->length) return false;
            editor->cursor = editor->length;
            return true;
        case '~':
            return size == 3 && sequence[1] == '3' && shell_line_editor__delete(editor);
        default: return false;
    }
}

static bool shell_console__handle_byte(
    shell_line_editor_t *editor, shell_history_browser_t *history, shell_console_tab_state_t *tab_state,
    unsigned char byte
) {
    switch (byte) {
        case '\b':
        case SHELL_CONSOLE_DELETE: return shell_line_editor__backspace(editor);
        case SHELL_CONSOLE_CTRL_A:
            if (editor->cursor == 0) return false;
            editor->cursor = 0;
            return true;
        case SHELL_CONSOLE_CTRL_E:
            if (editor->cursor == editor->length) return false;
            editor->cursor = editor->length;
            return true;
        case SHELL_CONSOLE_CTRL_U:
            if (editor->length == 0) return false;
            shell_line_editor__set(editor, "");
            return true;
        case '\t': return shell_console__complete(editor, tab_state);
        case SHELL_CONSOLE_ESCAPE: return shell_console__handle_escape(editor, history);
        default:
            if (byte < ' ' || byte > '~') return false;
            shell_history_browser__reset(history);
            return shell_line_editor__insert(editor, (char)byte);
    }
}

int shell_console__read_line(char *line, size_t capacity, bool *skip_lf) {
    shell_line_editor_t editor;
    shell_line_editor__init(&editor, line, capacity);
    size_t draft_capacity = capacity > 0 ? capacity : 1u;
    char *draft = memory__calloc(draft_capacity, 1u);
    if (draft == NULL) return BRUCE_ERR_NO_MEMORY;
    shell_history_browser_t history;
    shell_console_tab_state_t tab_state = {0};
    shell_history_browser__init(&history, draft, draft_capacity);
    shell_console__redraw(&editor);
    s_shell_console_ready = true;

    for (;;) {
        int input = shell_console__read_byte(UINT32_MAX);
        if (input < 0) {
            shell_console__tab_state_free(&tab_state);
            memory__free(draft);
            return input;
        }
        unsigned char byte = (unsigned char)input;
        if (*skip_lf && byte == '\n') {
            *skip_lf = false;
            continue;
        }
        *skip_lf = false;
        if (byte == '\r' || byte == '\n') {
            *skip_lf = byte == '\r';
            (void)stdio__write("\r\n", 2);
            shell_console__tab_state_free(&tab_state);
            memory__free(draft);
            return (int)editor.length;
        }
        bool redraw = shell_console__handle_byte(&editor, &history, &tab_state, byte);
        if (byte != '\t') shell_console__tab_reset(&tab_state);
        if (redraw) shell_console__redraw(&editor);
    }
}

void shell_console__reset_ready(void) { s_shell_console_ready = false; }

bool shell_console__is_ready(void) { return s_shell_console_ready; }
