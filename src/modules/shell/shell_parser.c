#include "shell_parser.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

bool shell_parser__valid_name(const char *name, size_t length) {
    if (name == NULL || length == 0 || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (size_t i = 1; i < length; ++i) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return false;
    }
    return true;
}

int shell_parser__plan(const char *line, shell_plan_t *plan, const char **error) {
    if (line == NULL || plan == NULL || error == NULL) return -1;
    memset(plan, 0, sizeof(*plan));
    *error = NULL;
    size_t length = strlen(line);
    size_t start = 0;
    shell_connector_t next_connector = SHELL_CONNECT_NONE;
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    bool token_boundary = true;

    for (size_t i = 0; i <= length; ++i) {
        char c = i < length ? line[i] : '\0';
        if (escaped) {
            escaped = false;
            token_boundary = false;
            continue;
        }
        if (!single && c == '\\') {
            if (i + 1 >= length) {
                *error = "trailing escape";
                return -1;
            }
            escaped = true;
            token_boundary = false;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            token_boundary = false;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            token_boundary = false;
            continue;
        }
        if (single || double_quote) continue;
        if (c == '#' && token_boundary) {
            length = i;
            c = '\0';
        }
        if (c == '|' && i + 1 < length && line[i + 1] == '|') {
            /* handled below as a supported connector */
        } else if (c == '|' || c == '<' || c == '>') {
            *error = "pipes and redirection are unsupported";
            return -1;
        }

        shell_connector_t connector = SHELL_CONNECT_NONE;
        size_t operator_size = 0;
        if (c == ';') {
            connector = SHELL_CONNECT_SEQUENCE;
            operator_size = 1;
        } else if (c == '&' && i + 1 < length && line[i + 1] == '&') {
            connector = SHELL_CONNECT_AND;
            operator_size = 2;
        } else if (c == '|' && i + 1 < length && line[i + 1] == '|') {
            connector = SHELL_CONNECT_OR;
            operator_size = 2;
        } else if (c == '&') {
            *error = "unsupported operator";
            return -1;
        }

        if (operator_size != 0 || c == '\0') {
            size_t end = i;
            while (start < end && isspace((unsigned char)line[start])) start++;
            while (end > start && isspace((unsigned char)line[end - 1])) end--;
            if (end > start) {
                if (plan->count >= SHELL__MAX_COMMANDS) {
                    *error = "too many commands";
                    return -1;
                }
                plan->commands[plan->count++] = (shell_command_t){
                    .text = line + start,
                    .length = end - start,
                    .connector = next_connector,
                };
                next_connector = SHELL_CONNECT_NONE;
            } else if (operator_size != 0 && (plan->count == 0 || next_connector != SHELL_CONNECT_NONE)) {
                *error = "unexpected operator";
                return -1;
            }
            if (operator_size != 0) {
                next_connector = connector;
                i += operator_size - 1;
                start = i + 1;
                token_boundary = true;
            }
        } else {
            token_boundary = isspace((unsigned char)c);
        }
    }
    if (single || double_quote) {
        *error = "unterminated quote";
        return -1;
    }
    if (plan->count > 0 && next_connector != SHELL_CONNECT_NONE && next_connector != SHELL_CONNECT_SEQUENCE) {
        *error = "missing command after operator";
        return -1;
    }
    return 0;
}

static bool shell_parser__append(char *word, size_t *used, const char *text, size_t length) {
    if (*used + length >= SHELL__WORD_MAX) return false;
    memcpy(word + *used, text, length);
    *used += length;
    word[*used] = '\0';
    return true;
}

static int shell_parser__expand(
    const shell_command_t *command, size_t *position, char *word, size_t *used,
    shell_variable_lookup_fn lookup, void *context, int last_status, const char **error
) {
    size_t i = *position;
    if (i + 1 >= command->length) {
        *position = i + 1;
        return shell_parser__append(word, used, "$", 1) ? 0 : -1;
    }
    char name[SHELL__PARSED_NAME_MAX];
    size_t name_length = 0;
    if (command->text[i + 1] == '?') {
        char status[12];
        int written = snprintf(status, sizeof(status), "%d", last_status);
        *position = i + 2;
        return written > 0 && shell_parser__append(word, used, status, (size_t)written) ? 0 : -1;
    }
    if (command->text[i + 1] == '{') {
        i += 2;
        while (i < command->length && command->text[i] != '}') {
            if (name_length + 1 >= sizeof(name)) {
                *error = "variable name too long";
                return -1;
            }
            name[name_length++] = command->text[i++];
        }
        if (i >= command->length || command->text[i] != '}') {
            *error = "unterminated variable expansion";
            return -1;
        }
        *position = i + 1;
    } else {
        i++;
        if (!(isalpha((unsigned char)command->text[i]) || command->text[i] == '_')) {
            *position = i;
            return shell_parser__append(word, used, "$", 1) ? 0 : -1;
        }
        while (i < command->length && (isalnum((unsigned char)command->text[i]) || command->text[i] == '_')) {
            if (name_length + 1 >= sizeof(name)) {
                *error = "variable name too long";
                return -1;
            }
            name[name_length++] = command->text[i++];
        }
        *position = i;
    }
    if (!shell_parser__valid_name(name, name_length)) {
        *error = "invalid variable name";
        return -1;
    }
    name[name_length] = '\0';
    const char *value = lookup != NULL ? lookup(context, name) : NULL;
    if (value != NULL && !shell_parser__append(word, used, value, strlen(value))) {
        *error = "expanded word too long";
        return -1;
    }
    return 0;
}

int shell_parser__words(
    const shell_command_t *command, char words[SHELL__MAX_WORDS][SHELL__WORD_MAX], int *word_count,
    shell_variable_lookup_fn lookup, void *lookup_context, int last_status, const char **error
) {
    if (command == NULL || words == NULL || word_count == NULL || error == NULL) return -1;
    *word_count = 0;
    *error = NULL;
    size_t i = 0;
    while (i < command->length) {
        while (i < command->length && isspace((unsigned char)command->text[i])) i++;
        if (i >= command->length) break;
        if (*word_count >= SHELL__MAX_WORDS) {
            *error = "too many words";
            return -1;
        }
        char *word = words[*word_count];
        size_t used = 0;
        bool single = false;
        bool double_quote = false;
        bool started = false;
        word[0] = '\0';
        while (i < command->length) {
            char c = command->text[i];
            if (!single && !double_quote && isspace((unsigned char)c)) break;
            started = true;
            if (!double_quote && c == '\'') {
                single = !single;
                i++;
                continue;
            }
            if (!single && c == '"') {
                double_quote = !double_quote;
                i++;
                continue;
            }
            if (!single && c == '\\') {
                i++;
                if (i >= command->length || !shell_parser__append(word, &used, command->text + i, 1)) {
                    *error = "word too long";
                    return -1;
                }
                i++;
                continue;
            }
            if (!single && c == '$') {
                if (shell_parser__expand(
                        command, &i, word, &used, lookup, lookup_context, last_status, error
                    ) != 0) {
                    if (*error == NULL) *error = "expanded word too long";
                    return -1;
                }
                continue;
            }
            if (!shell_parser__append(word, &used, command->text + i, 1)) {
                *error = "word too long";
                return -1;
            }
            i++;
        }
        if (single || double_quote) {
            *error = "unterminated quote";
            return -1;
        }
        if (started) (*word_count)++;
    }
    return 0;
}
