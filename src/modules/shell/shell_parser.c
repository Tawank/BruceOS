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

/* Finds the end of a "$(...)" command-substitution span (`backtick` false,
 * `start` pointing at the '$' with text[start+1] == '(') or a "`...`" one
 * (`backtick` true, `start` pointing at the opening '`'), setting *out_end
 * to the index just past the matching ')' or closing '`' and returning true
 * -- or returning false (an unterminated span) if the matching close is
 * never found before `length`. Tracks nested "$(...)" spans and bare
 * '('/')' pairs inside a "$(...)" the same way shell_parser__plan()'s own
 * arith_depth already tracks "((...))", and skips quoted content within the
 * span (so a stray ')'/'`' inside a quoted string in there doesn't end the
 * span early), matching shell_parser__plan()'s own top-level quote
 * handling. Used both to find where a substitution's content ends
 * (shell_parser__expand()/shell_parser__words() below) and, by
 * shell_parser__plan()/shell_parser__extract_redirect(), to skip over the
 * whole span as one opaque unit so ';'/'&&'/'||'/'|'/'>' used *inside* it
 * (e.g. "$(cmd > file)") are never mistaken for this line's own operators. */
static bool
shell_parser__substitution_span(const char *text, size_t length, size_t start, bool backtick, size_t *out_end) {
    size_t i = start + (backtick ? 1u : 2u);
    int depth = 1;
    bool single = false, double_quote = false, escaped = false;
    while (i < length) {
        char c = text[i];
        if (escaped) {
            escaped = false;
            i++;
            continue;
        }
        if (!single && c == '\\') {
            escaped = true;
            i++;
            continue;
        }
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
        if (single || double_quote) {
            i++;
            continue;
        }
        if (backtick) {
            if (c == '`') {
                *out_end = i + 1;
                return true;
            }
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                *out_end = i + 1;
                return true;
            }
        }
        i++;
    }
    return false;
}

/* Parses the "<<"/"<<-" heredoc marker starting at text[start] (caller has
 * already confirmed text[start] == '<' && text[start + 1] == '<') -- the
 * optional '-' and the delimiter word that follows on the same line (bare,
 * or single-/double-quoted -- quoting only controls *out_literal, it doesn't
 * change how the word itself is delimited). Sets *out_end to the index just
 * past the whole clause. Shared by shell_parser__find_heredoc_marker() below
 * (a lone top-level scan over one raw physical line, before it ever reaches
 * shell_parser__plan()) and shell_parser__extract_redirect() further down
 * (the real, full parse once shell_app.c has already joined physical lines
 * into a block and collected any heredoc bodies) so the two agree on exactly
 * what counts as a valid marker. Returns false with *error set for a
 * malformed one ("<<" with nothing after it, or an unterminated quote in the
 * delimiter). A bare delimiter word ends at whitespace or at another shell
 * operator character, so "cmd <<EOF>x" (delimiter "EOF", then a plain output
 * redirect) parses the way bash's own lexer would. */
static bool shell_parser__scan_heredoc_marker(
    const char *text, size_t length, size_t start, bool *out_strip_tabs, bool *out_literal, const char **out_delim,
    size_t *out_delim_len, size_t *out_end, const char **error
) {
    size_t i = start + 2;
    bool strip_tabs = i < length && text[i] == '-';
    if (strip_tabs) i++;
    while (i < length && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i >= length || text[i] == '\n') {
        *error = "missing heredoc delimiter";
        return false;
    }
    bool literal = false;
    const char *delim_start;
    size_t delim_len;
    if (text[i] == '\'' || text[i] == '"') {
        char quote = text[i++];
        delim_start = text + i;
        while (i < length && text[i] != quote && text[i] != '\n') i++;
        if (i >= length || text[i] != quote) {
            *error = "unterminated quote in heredoc delimiter";
            return false;
        }
        delim_len = (size_t)((text + i) - delim_start);
        i++;
        literal = true;
    } else {
        delim_start = text + i;
        while (i < length && !isspace((unsigned char)text[i]) && text[i] != ';' && text[i] != '&' &&
               text[i] != '|' && text[i] != '<' && text[i] != '>') {
            i++;
        }
        delim_len = (size_t)((text + i) - delim_start);
    }
    if (delim_len == 0) {
        *error = "missing heredoc delimiter";
        return false;
    }
    *out_strip_tabs = strip_tabs;
    *out_literal = literal;
    *out_delim = delim_start;
    *out_delim_len = delim_len;
    *out_end = i;
    return true;
}

bool shell_parser__find_heredoc_marker(
    const char *line, size_t length, bool *out_strip_tabs, bool *out_literal, const char **out_delim,
    size_t *out_delim_len, const char **error
) {
    *error = NULL;
    bool single = false, double_quote = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char c = line[i];
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
        if ((c == '$' && i + 1 < length && line[i + 1] == '(') || c == '`') {
            size_t end = 0;
            if (!shell_parser__substitution_span(line, length, i, c == '`', &end)) {
                *error = "unterminated command substitution";
                return false;
            }
            i = end - 1;
            continue;
        }
        if (c == '<' && i + 1 < length && line[i + 1] == '<') {
            size_t end = 0;
            return shell_parser__scan_heredoc_marker(
                line, length, i, out_strip_tabs, out_literal, out_delim, out_delim_len, &end, error
            );
        }
    }
    return false;
}

/* Detects trailing "< target" / "> target" / ">> target" redirections (at
 * most one input and one output, in either order) and a trailing
 * "<<DELIM"/"<<-DELIM" heredoc marker on an already-trimmed command span
 * and, if any are found, shrinks command->length to exclude them, leaving
 * the plain command/arguments shell_parser__words() will tokenize normally.
 * A heredoc marker consumes the next entry of `heredoc_bodies` in order (see
 * shell_parser__plan()'s own doc comment on that parameter) -- reported as
 * "heredoc ... not supported here" if none is left. Anything beyond that set
 * (a second '<', a second '>', a second heredoc, or non-whitespace after a
 * target) is a syntax error rather than being silently misinterpreted. A
 * '<'/'>' inside a quoted word or a "((...))" arithmetic span (e.g.
 * "(( a > b ))") is never mistaken for an operator, via the same quote- and
 * arith-depth tracking shell_parser__plan()'s own scan uses. */
static int shell_parser__extract_redirect(
    shell_command_t *command, char *const *heredoc_bodies, size_t heredoc_count, size_t *heredoc_index,
    const char **error
) {
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
        if ((c == '$' && i + 1 < length && text[i + 1] == '(') || c == '`') {
            size_t end = 0;
            if (!shell_parser__substitution_span(text, length, i, c == '`', &end)) {
                *error = "unterminated command substitution";
                return -1;
            }
            i = end - 1;
            continue;
        }
        if (arith_depth == 0 && c == '(' && i + 1 < length && text[i + 1] == '(') {
            arith_depth = 1;
            continue;
        }
        if (arith_depth > 0) {
            if (c == '(') arith_depth++;
            else if (c == ')') arith_depth--;
            continue;
        }
        if (c == '<' || c == '>') {
            op_start = i;
            break;
        }
    }
    if (op_start == length) return 0; /* no redirection on this command */

    size_t i = op_start;
    bool seen_input = false, seen_output = false, seen_heredoc = false;
    for (;;) {
        char op = text[i];
        if (op == '<' && i + 1 < length && text[i + 1] == '<') {
            if (seen_heredoc) {
                *error = "multiple heredocs are unsupported";
                return -1;
            }
            seen_heredoc = true;
            bool strip_tabs = false, literal = false;
            const char *delim = NULL;
            size_t delim_len = 0, end = 0;
            if (!shell_parser__scan_heredoc_marker(text, length, i, &strip_tabs, &literal, &delim, &delim_len, &end, error)) {
                return -1;
            }
            (void)strip_tabs; /* only mattered while collecting the body; the result is already final text */
            (void)literal;
            (void)delim;
            (void)delim_len;
            if (*heredoc_index >= heredoc_count) {
                *error = "heredoc requires the enclosing script to have already read its body (not supported here)";
                return -1;
            }
            command->heredoc_body = heredoc_bodies[(*heredoc_index)++];
            i = end;
        } else if (op == '<') {
            if (seen_input) {
                *error = "multiple input redirections are unsupported";
                return -1;
            }
            seen_input = true;
            i += 1;
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
            command->input_redirect = true;
            command->input_target.text = text + target_start;
            command->input_target.length = i - target_start;
        } else {
            bool append = i + 1 < length && text[i + 1] == '>';
            if (seen_output) {
                *error = "multiple output redirections are unsupported";
                return -1;
            }
            seen_output = true;
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
            command->redirect = append ? SHELL_REDIRECT_APPEND : SHELL_REDIRECT_OUT;
            command->redirect_target.text = text + target_start;
            command->redirect_target.length = i - target_start;
        }
        while (i < length && isspace((unsigned char)text[i])) i++;
        if (i < length && (text[i] == '<' || text[i] == '>')) continue;
        break;
    }
    if (i < length) {
        *error = "unexpected text after redirection";
        return -1;
    }
    if (seen_input && seen_heredoc) {
        *error = "cannot combine '<' and a heredoc on the same command";
        return -1;
    }
    command->length = op_start;
    while (command->length > 0 && isspace((unsigned char)command->text[command->length - 1])) command->length--;
    return 0;
}

int shell_parser__plan(
    const char *line, shell_plan_t *plan, char *const *heredoc_bodies, size_t heredoc_count, const char **error
) {
    if (line == NULL || plan == NULL || error == NULL) return -1;
    memset(plan, 0, sizeof(*plan));
    *error = NULL;
    size_t length = strlen(line);
    size_t start = 0;
    size_t heredoc_index = 0;
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
        if ((c == '$' && i + 1 < length && line[i + 1] == '(') || c == '`') {
            size_t end = 0;
            if (!shell_parser__substitution_span(line, length, i, c == '`', &end)) {
                *error = "unterminated command substitution";
                return -1;
            }
            i = end - 1;
            token_boundary = false;
            continue;
        }
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
                if (shell_parser__extract_redirect(&command, heredoc_bodies, heredoc_count, &heredoc_index, error) != 0) {
                    return -1;
                }
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

/* Splices `substitute`'s captured output for the command-substitution span
 * command->text[content_start .. content_start+content_len) into `word`.
 * Shared by shell_parser__expand()'s "$(...)" branch below and
 * shell_parser__words()'s own "`...`" handling further down. */
static int shell_parser__splice_substitution(
    const shell_command_t *command, size_t content_start, size_t content_len, shell_word_buffer_t *word,
    shell_command_substitution_fn substitute, void *context, const char **error
) {
    char *result = substitute != NULL ? substitute(context, command->text + content_start, content_len) : NULL;
    if (result == NULL) {
        *error = "command substitution failed";
        return -1;
    }
    bool appended = shell_word_buffer__append(word, result, strlen(result));
    memory__free(result);
    if (!appended) *error = "expanded word too long";
    return appended ? 0 : -1;
}

/* Splices `arith`'s formatted result for the "$((...))" arithmetic-word span
 * command->text[inner_start .. inner_start+inner_len) -- the expression
 * between the doubled "((" and "))", *not* including those four characters
 * -- into `word`. Mirrors shell_parser__splice_substitution() above, except
 * a failure propagates `arith`'s own specific, caller-durable error message
 * instead of a fixed generic one (matching how the standalone "((...))"
 * statement form reports arithmetic errors -- see shell_executor.c). */
static int shell_parser__splice_arith(
    const shell_command_t *command, size_t inner_start, size_t inner_len, shell_word_buffer_t *word,
    shell_arith_word_fn arith, void *context, const char **error
) {
    const char *arith_error = NULL;
    char *result =
        arith != NULL ? arith(context, command->text + inner_start, inner_len, &arith_error) : NULL;
    if (result == NULL) {
        *error = arith_error != NULL ? arith_error : "arithmetic expansion failed";
        return -1;
    }
    bool appended = shell_word_buffer__append(word, result, strlen(result));
    memory__free(result);
    if (!appended) *error = "expanded word too long";
    return appended ? 0 : -1;
}

static int shell_parser__expand(
    const shell_command_t *command, size_t *position, shell_word_buffer_t *word, shell_variable_lookup_fn lookup,
    shell_command_substitution_fn substitute, shell_arith_word_fn arith, void *context, int last_status,
    const char **error
) {
    size_t i = *position;
    if (i + 1 >= command->length) {
        *position = i + 1;
        return shell_word_buffer__append(word, "$", 1) ? 0 : -1;
    }
    /* "$(...)" command substitution -- see shell_command_substitution_fn in
     * shell_parser.h. Its content span was already validated as
     * well-terminated by shell_parser__plan()'s own scan of the whole line
     * before shell_parser__words() ever runs on one command out of it, so
     * shell_parser__substitution_span() failing here would mean those two
     * scans disagreed; still checked and reported rather than assumed.
     *
     * "$((...))" arithmetic expansion is recognized here too, as a special
     * case of the very same span: since this shell has no subshell "(...)"
     * command grouping (see README.md's "Not implemented" list) to
     * disambiguate against, a content span itself wrapped in one more
     * matched pair of parens -- e.g. "(1 + 2)" inside "$((1 + 2))" -- is
     * unambiguously the doubled-paren arithmetic form and never a real
     * command starting with a literal '(' word. */
    if (command->text[i + 1] == '(') {
        size_t end = 0;
        if (!shell_parser__substitution_span(command->text, command->length, i, false, &end)) {
            *error = "unterminated command substitution";
            return -1;
        }
        size_t content_start = i + 2;
        size_t content_len = (end - 1) - content_start;
        *position = end;
        if (content_len >= 2 && command->text[content_start] == '(' &&
            command->text[content_start + content_len - 1] == ')') {
            return shell_parser__splice_arith(
                command, content_start + 1, content_len - 2, word, arith, context, error
            );
        }
        return shell_parser__splice_substitution(command, content_start, content_len, word, substitute, context, error);
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

/* Growable text buffer for shell_parser__expand_text() below -- same
 * doubling-capacity growth as shell_word_buffer_t above, just capped at the
 * bigger SHELL__HEREDOC_MAX instead of SHELL__WORD_MAX, since a heredoc body
 * is prose/data rather than one argv word. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} shell_text_buffer_t;

static bool shell_text_buffer__append(shell_text_buffer_t *buf, const char *text, size_t length) {
    size_t needed = buf->length + length + 1;
    if (needed > SHELL__HEREDOC_MAX) return false;
    if (needed > buf->capacity) {
        size_t new_capacity = buf->capacity == 0 ? 128 : buf->capacity;
        while (new_capacity < needed) new_capacity *= 2;
        if (new_capacity > SHELL__HEREDOC_MAX) new_capacity = SHELL__HEREDOC_MAX;
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

char *shell_parser__expand_text(
    const char *text, size_t length, shell_variable_lookup_fn lookup, shell_command_substitution_fn substitute,
    shell_arith_word_fn arith, void *context, int last_status, const char **error
) {
    shell_command_t pseudo = {.text = text, .length = length};
    shell_text_buffer_t out = {0};
    size_t i = 0;
    while (i < length) {
        char c = text[i];
        /* Unlike shell_parser__words()'s general backslash handling, a
         * heredoc body only treats backslash specially right before these
         * four bytes (see this function's own doc comment in shell_parser.h
         * for why) -- everything else, escaped or not, is copied as-is. */
        if (c == '\\' && i + 1 < length &&
            (text[i + 1] == '$' || text[i + 1] == '`' || text[i + 1] == '\\' || text[i + 1] == '\n')) {
            if (text[i + 1] != '\n' && !shell_text_buffer__append(&out, &text[i + 1], 1)) {
                *error = "heredoc body too long";
                memory__free(out.data);
                return NULL;
            }
            i += 2;
            continue;
        }
        if (c == '$') {
            shell_word_buffer_t word = {0};
            if (shell_parser__expand(&pseudo, &i, &word, lookup, substitute, arith, context, last_status, error) !=
                0) {
                if (*error == NULL) *error = "expanded heredoc body too long";
                memory__free(word.data);
                memory__free(out.data);
                return NULL;
            }
            bool appended = word.length == 0 || shell_text_buffer__append(&out, word.data, word.length);
            memory__free(word.data);
            if (!appended) {
                *error = "heredoc body too long";
                memory__free(out.data);
                return NULL;
            }
            continue;
        }
        if (!shell_text_buffer__append(&out, &c, 1)) {
            *error = "heredoc body too long";
            memory__free(out.data);
            return NULL;
        }
        i++;
    }
    if (out.data != NULL) return out.data;
    /* An empty (or entirely-escaped-away) body still needs a real, owned,
     * empty string -- shell_text_buffer__append() never allocates for a
     * zero-length append, so `out.data` is still NULL at this point. */
    char *empty = memory__malloc(1);
    if (empty != NULL) empty[0] = '\0';
    else *error = "out of memory";
    return empty;
}

int shell_parser__words(
    const shell_command_t *command, char ***out_words, int *word_count, shell_variable_lookup_fn lookup,
    shell_command_substitution_fn substitute, shell_arith_word_fn arith, void *context, int last_status,
    const char **error
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
                if (shell_parser__expand(command, &i, &word, lookup, substitute, arith, context, last_status, error) !=
                    0) {
                    if (*error == NULL) *error = "expanded word too long";
                    memory__free(word.data);
                    shell_word_list__free(&list);
                    return -1;
                }
                continue;
            }
            /* "`...`" command substitution -- same expansion as "$(...)"
             * above (see shell_command_substitution_fn in shell_parser.h),
             * just with the other spelling bash accepts. Recognized inside
             * double quotes too, same as "$(...)"/"$NAME", only single
             * quotes suppress it. */
            if (!single && c == '`') {
                size_t end = 0;
                if (!shell_parser__substitution_span(command->text, command->length, i, true, &end)) {
                    *error = "unterminated command substitution";
                    memory__free(word.data);
                    shell_word_list__free(&list);
                    return -1;
                }
                size_t content_start = i + 1;
                size_t content_len = (end - 1) - content_start;
                if (shell_parser__splice_substitution(command, content_start, content_len, &word, substitute, context, error) !=
                    0) {
                    memory__free(word.data);
                    shell_word_list__free(&list);
                    return -1;
                }
                i = end;
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
