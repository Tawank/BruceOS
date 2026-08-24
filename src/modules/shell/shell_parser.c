#include "shell_parser.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"

static bool shell_parser__plan_push(shell_plan_t *plan, const shell_command_t *command) {
    if (plan->count >= SHELL__MAX_COMMANDS) return false;
    if (plan->count >= plan->capacity) {
        size_t new_capacity = plan->capacity == 0 ? 4u : plan->capacity * 2u;
        if (new_capacity > SHELL__MAX_COMMANDS) new_capacity = SHELL__MAX_COMMANDS;
        shell_command_t *grown = memory__realloc(plan->commands, new_capacity * sizeof(*grown));
        if (grown == NULL) return false;
        plan->commands = grown;
        plan->capacity = new_capacity;
    }
    plan->commands[plan->count++] = *command;
    return true;
}

void shell_parser__plan_free(shell_plan_t *plan) {
    if (plan == NULL) return;
    memory__free(plan->commands);
    plan->commands = NULL;
    plan->count = 0;
    plan->capacity = 0;
}

bool shell_parser__valid_name(const char *name, size_t length) {
    if (name == NULL || length == 0 || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (size_t i = 1; i < length; ++i) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return false;
    }
    return true;
}

/* Detects a single trailing "> target" / ">> target" output redirection on
 * an already-trimmed command span and, if found, shrinks command->length to
 * exclude it, leaving the plain command/arguments shell_parser__words() will
 * tokenize normally. Only a single redirection at the very end of the
 * command is supported -- anything else (a second '>', or non-whitespace
 * after the target) is a syntax error rather than being silently
 * misinterpreted. A '>' inside a quoted word or a "((...))" arithmetic span
 * (e.g. "(( a > b ))") is never mistaken for the operator, via the same
 * quote- and arith-depth tracking shell_parser__plan()'s own scan uses. */
static int shell_parser__extract_redirect(shell_command_t *command, const char **error) {
    const char *text = command->text;
    size_t length = command->length;
    bool single = false, double_quote = false, escaped = false;
    int arith_depth = 0;
    size_t op_start = length;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (!single && c == '\\') {
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (single || double_quote) continue;
        if (arith_depth == 0 && c == '(' && i + 1 < length && text[i + 1] == '(') {
            arith_depth = 1;
            continue;
        }
        if (arith_depth > 0) {
            if (c == '(') arith_depth++;
            else if (c == ')') arith_depth--;
            continue;
        }
        if (c == '>') {
            op_start = i;
            break;
        }
    }
    if (op_start == length) return 0; /* no redirection on this command */

    size_t i = op_start;
    bool append = i + 1 < length && text[i + 1] == '>';
    i += append ? 2u : 1u;
    while (i < length && isspace((unsigned char)text[i])) i++;
    if (i >= length) {
        *error = "missing redirection target";
        return -1;
    }
    size_t target_start = i;
    bool t_single = false, t_double = false, t_escaped = false;
    while (i < length) {
        char c = text[i];
        if (t_escaped) {
            t_escaped = false;
            i++;
            continue;
        }
        if (!t_single && c == '\\') {
            t_escaped = true;
            i++;
            continue;
        }
        if (!t_double && c == '\'') {
            t_single = !t_single;
            i++;
            continue;
        }
        if (!t_single && c == '"') {
            t_double = !t_double;
            i++;
            continue;
        }
        if (!t_single && !t_double && isspace((unsigned char)c)) break;
        i++;
    }
    if (t_single || t_double) {
        *error = "unterminated quote in redirection target";
        return -1;
    }
    if (i == target_start) {
        *error = "missing redirection target";
        return -1;
    }
    size_t target_end = i;
    while (i < length && isspace((unsigned char)text[i])) i++;
    if (i < length) {
        *error =
            text[i] == '>' ? "multiple output redirections are unsupported" : "unexpected text after redirection";
        return -1;
    }
    command->redirect = append ? SHELL_REDIRECT_APPEND : SHELL_REDIRECT_OUT;
    command->redirect_target.text = text + target_start;
    command->redirect_target.length = target_end - target_start;
    command->length = op_start;
    while (command->length > 0 && isspace((unsigned char)command->text[command->length - 1])) command->length--;
    return 0;
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
    /* Set while skipping a "# ..." comment. Unlike the rest of this scanner,
     * a comment isn't scoped to one physical line -- shell_compound.c feeds
     * whole multi-line if/fi and function blocks through here in one call,
     * with real '\n' bytes between the original lines (see the '\n'
     * connector case below) -- so a comment must stop at the next '\n'
     * rather than swallowing every statement after it in the block. */
    bool in_comment = false;
    /* Tracks a "((...))" arithmetic-command span (see shell_arith.c and
     * shell_compound__run_for()'s C-style header) so ';'/'&&'/'||'/'|'/'<'/
     * '>' inside one -- as in `for ((i=0;i<10;i++))` or `(( a < b ))` -- are
     * left as plain text instead of being parsed as shell operators or
     * rejected as redirection. Only a literal doubled "((" opens a span,
     * matching bash's own recognition of the construct; once open,
     * arith_depth just counts unmatched '(' (so a nested single-paren
     * group, e.g. "(( (a+b)*c ))", doesn't close it early) until it returns
     * to 0 at the matching "))". */
    int arith_depth = 0;

    for (size_t i = 0; i <= length; ++i) {
        char c = i < length ? line[i] : '\0';
        if (in_comment) {
            if (c != '\n' && c != '\0') continue;
            in_comment = false;
        }
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
            in_comment = true;
            continue;
        }
        if (arith_depth == 0 && c == '(' && i + 1 < length && line[i + 1] == '(') {
            /* This is the first '(' of the doubled "((" opener; the second
             * one is counted by the ordinary +1 below when the next loop
             * iteration reaches it -- setting arith_depth to 2 here too
             * would double-count it. */
            arith_depth = 1;
        } else if (arith_depth > 0) {
            if (c == '(') arith_depth++;
            else if (c == ')') arith_depth--;
        }
        if (arith_depth > 0 && c != '\0') {
            /* Never swallow the end-of-buffer sentinel here: even an
             * unterminated "((" must still fall through to the flush logic
             * below so the last command isn't silently dropped. */
            token_boundary = isspace((unsigned char)c);
            continue;
        }
        if (c == '<') {
            *error = "input redirection ('<') is unsupported";
            return -1;
        }

        shell_connector_t connector = SHELL_CONNECT_NONE;
        size_t operator_size = 0;
        if (c == ';' || c == '\n') {
            connector = SHELL_CONNECT_SEQUENCE;
            operator_size = 1;
        } else if (c == '&' && i + 1 < length && line[i + 1] == '&') {
            connector = SHELL_CONNECT_AND;
            operator_size = 2;
        } else if (c == '|' && i + 1 < length && line[i + 1] == '|') {
            connector = SHELL_CONNECT_OR;
            operator_size = 2;
        } else if (c == '|') {
            connector = SHELL_CONNECT_PIPE;
            operator_size = 1;
        } else if (c == '&') {
            *error = "unsupported operator";
            return -1;
        }

        if (operator_size != 0 || c == '\0') {
            size_t end = i;
            while (start < end && isspace((unsigned char)line[start])) start++;
            while (end > start && isspace((unsigned char)line[end - 1])) end--;
            if (end > start) {
                shell_command_t command = {
                    .text = line + start,
                    .length = end - start,
                    .connector = next_connector,
                };
                if (shell_parser__extract_redirect(&command, error) != 0) return -1;
                if (plan->count >= SHELL__MAX_COMMANDS) {
                    *error = "too many commands";
                    return -1;
                }
                if (!shell_parser__plan_push(plan, &command)) {
                    *error = "out of memory";
                    return -1;
                }
                next_connector = SHELL_CONNECT_NONE;
            } else if (
                operator_size != 0 && c != '\n' && (plan->count == 0 || next_connector != SHELL_CONNECT_NONE)
            ) {
                *error = "unexpected operator";
                return -1;
            }
            if (operator_size != 0) {
                /* A blank line, or a '\n' right after another separator
                 * (";\n", a blank line between statements, "&&\n" line
                 * continuation into the next physical line, ...) carries no
                 * command of its own -- unlike an explicit ';'/'&&'/'||'/'|'
                 * with nothing before it, which is a real syntax error, this
                 * is just whitespace and must not downgrade or clobber
                 * whatever real connector (if any) is already pending. */
                if (c != '\n' || next_connector == SHELL_CONNECT_NONE) next_connector = connector;
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

/* Grows on demand (starting small) instead of preallocating SHELL__WORD_MAX,
 * so a typical short word costs a fraction of the worst case. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} shell_word_buffer_t;

static bool shell_word_buffer__append(shell_word_buffer_t *buf, const char *text, size_t length) {
    size_t needed = buf->length + length + 1;
    if (needed > SHELL__WORD_MAX) return false;
    if (needed > buf->capacity) {
        size_t new_capacity = buf->capacity == 0 ? 32 : buf->capacity;
        while (new_capacity < needed) new_capacity *= 2;
        if (new_capacity > SHELL__WORD_MAX) new_capacity = SHELL__WORD_MAX;
        char *grown = memory__realloc(buf->data, new_capacity);
        if (grown == NULL) return false;
        buf->data = grown;
        buf->capacity = new_capacity;
    }
    memcpy(buf->data + buf->length, text, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
    return true;
}

/* Releases any capacity beyond what the finished word actually needs, and
 * turns a never-appended-to buffer (e.g. from a bare `''`) into an owned
 * empty string so callers always get a valid pointer. */
static char *shell_word_buffer__finish(shell_word_buffer_t *buf) {
    if (buf->data == NULL) {
        char *empty = memory__malloc(1);
        if (empty != NULL) empty[0] = '\0';
        return empty;
    }
    char *shrunk = memory__realloc(buf->data, buf->length + 1);
    return shrunk != NULL ? shrunk : buf->data;
}

typedef struct {
    char **items;
    int count;
    int capacity;
} shell_word_list_t;

static bool shell_word_list__push(shell_word_list_t *list, char *word) {
    if (list->count >= SHELL__MAX_WORDS) return false;
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        if (new_capacity > SHELL__MAX_WORDS) new_capacity = SHELL__MAX_WORDS;
        char **grown = memory__realloc(list->items, (size_t)new_capacity * sizeof(*grown));
        if (grown == NULL) return false;
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = word;
    return true;
}

static void shell_word_list__free(shell_word_list_t *list) {
    for (int i = 0; i < list->count; ++i) memory__free(list->items[i]);
    memory__free(list->items);
}

static int shell_parser__expand(
    const shell_command_t *command, size_t *position, shell_word_buffer_t *word, shell_variable_lookup_fn lookup,
    void *context, int last_status, const char **error
) {
    size_t i = *position;
    if (i + 1 >= command->length) {
        *position = i + 1;
        return shell_word_buffer__append(word, "$", 1) ? 0 : -1;
    }
    char name[SHELL__PARSED_NAME_MAX];
    size_t name_length = 0;
    if (command->text[i + 1] == '?') {
        char status[12];
        int written = snprintf(status, sizeof(status), "%d", last_status);
        *position = i + 2;
        return written > 0 && shell_word_buffer__append(word, status, (size_t)written) ? 0 : -1;
    }
    /* $0/$1../$9/$# -- a function call's name, its positional parameters,
     * and how many of them there are (see shell_compound__call_function()
     * in shell_compound.c, and shell_executor__lookup() in shell_executor.c,
     * which is what actually resolves these through `lookup`). Bash itself
     * only ever expands a single digit unbraced this way ($10 is $1 followed
     * by a literal "0"), so this doesn't loop to collect more digits. An
     * unset positional parameter expands to nothing, same as an unset named
     * variable below. */
    if (command->text[i + 1] == '#' || isdigit((unsigned char)command->text[i + 1])) {
        char key[2] = {command->text[i + 1], '\0'};
        *position = i + 2;
        const char *value = lookup != NULL ? lookup(context, key) : NULL;
        return value == NULL || shell_word_buffer__append(word, value, strlen(value)) ? 0 : -1;
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
            return shell_word_buffer__append(word, "$", 1) ? 0 : -1;
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
    if (value != NULL && !shell_word_buffer__append(word, value, strlen(value))) {
        *error = "expanded word too long";
        return -1;
    }
    return 0;
}

int shell_parser__words(
    const shell_command_t *command, char ***out_words, int *word_count, shell_variable_lookup_fn lookup,
    void *lookup_context, int last_status, const char **error
) {
    if (command == NULL || out_words == NULL || word_count == NULL || error == NULL) return -1;
    *out_words = NULL;
    *word_count = 0;
    *error = NULL;
    shell_word_list_t list = {0};
    size_t i = 0;
    while (i < command->length) {
        while (i < command->length && isspace((unsigned char)command->text[i])) i++;
        if (i >= command->length) break;

        shell_word_buffer_t word = {0};
        bool single = false;
        bool double_quote = false;
        bool started = false;
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
                if (i >= command->length || !shell_word_buffer__append(&word, command->text + i, 1)) {
                    *error = "word too long";
                    memory__free(word.data);
                    shell_word_list__free(&list);
                    return -1;
                }
                i++;
                continue;
            }
            if (!single && c == '$') {
                if (shell_parser__expand(command, &i, &word, lookup, lookup_context, last_status, error) != 0) {
                    if (*error == NULL) *error = "expanded word too long";
                    memory__free(word.data);
                    shell_word_list__free(&list);
                    return -1;
                }
                continue;
            }
            if (!shell_word_buffer__append(&word, command->text + i, 1)) {
                *error = "word too long";
                memory__free(word.data);
                shell_word_list__free(&list);
                return -1;
            }
            i++;
        }
        if (single || double_quote) {
            *error = "unterminated quote";
            memory__free(word.data);
            shell_word_list__free(&list);
            return -1;
        }
        if (started) {
            char *finished = shell_word_buffer__finish(&word);
            if (finished == NULL || !shell_word_list__push(&list, finished)) {
                bool too_many = finished != NULL && list.count >= SHELL__MAX_WORDS;
                memory__free(finished);
                *error = too_many ? "too many words" : "out of memory";
                shell_word_list__free(&list);
                return -1;
            }
        }
    }
    *out_words = list.items;
    *word_count = list.count;
    return 0;
}

void shell_parser__free_words(char **words, int word_count) {
    if (words == NULL) return;
    for (int i = 0; i < word_count; ++i) memory__free(words[i]);
    memory__free(words);
}
