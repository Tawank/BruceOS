#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SHELL__MAX_COMMANDS 32
/* Safety caps against runaway expansions, not preallocation sizes. */
#define SHELL__MAX_WORDS 24
#define SHELL__WORD_MAX 256
#define SHELL__PARSED_NAME_MAX 32
/* Safety cap on a "<<DELIM" heredoc body's expanded size (see
 * shell_parser__expand_text() below) -- deliberately bigger than
 * SHELL__WORD_MAX since a heredoc body is prose/data, not one argv word. */
#define SHELL__HEREDOC_MAX 4096

typedef enum {
    SHELL_CONNECT_NONE,
    SHELL_CONNECT_SEQUENCE,
    SHELL_CONNECT_AND,
    SHELL_CONNECT_OR,
    SHELL_CONNECT_PIPE,
} shell_connector_t;

/* A single trailing ">"/">>" output redirection recognized on one command by
 * shell_parser__plan() -- see its shell_parser__extract_redirect() helper.
 * Input redirection ('<') is tracked separately (shell_command_t's own
 * `input_redirect`/`input_target` below), since a command can carry one of
 * each direction at once (e.g. "sort < in.txt > out.txt"). */
typedef enum {
    SHELL_REDIRECT_NONE = 0,
    SHELL_REDIRECT_OUT,    /* > : truncate/create */
    SHELL_REDIRECT_APPEND, /* >> : append/create */
} shell_redirect_t;

/* A raw, unexpanded span into the original line -- e.g. a redirection
 * target -- expanded the same way an ordinary argv word is, by wrapping it
 * in a one-off shell_command_t and running it through shell_parser__words(). */
typedef struct {
    const char *text;
    size_t length;
} shell_word_span_t;

typedef struct {
    const char *text;
    size_t length;
    shell_connector_t connector;
    shell_redirect_t redirect;
    shell_word_span_t redirect_target;
    /* "< target" -- see shell_parser__extract_redirect() in shell_parser.c.
     * Combinable with `redirect` above (one of each direction), but not with
     * `heredoc_body` below (both are ways of sourcing this command's stdin). */
    bool input_redirect;
    shell_word_span_t input_target;
    /* "<<DELIM"/"<<-DELIM" -- NULL when this command has no heredoc. Unlike
     * every other field here this is *not* a span into `text`: by the time
     * shell_parser__plan() sees this command, the body itself has already
     * been read (from wherever `text` came from) and fully resolved --
     * expanded (unless the delimiter was quoted) and NUL-terminated -- by
     * the caller that collected it (see shell_parser__plan()'s own
     * `heredoc_bodies` parameter and shell_app.c's shell__run_script(),
     * the only place that currently supplies any). This command borrows the
     * pointer; it does not own or free it. */
    const char *heredoc_body;
} shell_command_t;

typedef struct {
    shell_command_t *commands;
    size_t count;
    size_t capacity;
} shell_plan_t;

typedef const char *(*shell_variable_lookup_fn)(void *context, const char *name);

/* Runs `command_text` (`length` bytes, not NUL-terminated) as a nested shell
 * command list for "$(...)" / "`...`" command substitution and returns its
 * captured, trailing-newlines-stripped stdout as a heap-allocated (same
 * allocator shell_parser__free_words() releases its items with --
 * memory__malloc()), NUL-terminated string the caller owns, or NULL on
 * failure (out of memory, or `command_text` too long to run at all) -- see
 * shell_executor__run_substitution() in shell_executor.c for what actually
 * implements this. */
typedef char *(*shell_command_substitution_fn)(void *context, const char *command_text, size_t length);

/* Evaluates `text` (`length` bytes, not NUL-terminated) -- the expression
 * between the doubled "((" and "))" of a "$((...))" arithmetic-expansion
 * *word* (e.g. the "1 + 2" in "echo $((1 + 2))") -- and returns its result
 * formatted as a decimal string, heap-allocated the same way
 * shell_command_substitution_fn's result is (memory__malloc(), owned by the
 * caller). On failure (a syntax error, division by zero, ...) returns NULL
 * and sets *error to a human-readable, caller-durable (string-literal)
 * message -- see shell_arith__eval() in shell_arith.c, which is what
 * actually implements this, for what those messages look like. */
typedef char *(*shell_arith_word_fn)(void *context, const char *text, size_t length, const char **error);

/* `heredoc_bodies`/`heredoc_count` are the already-collected, already-expanded
 * heredoc bodies for every "<<DELIM"/"<<-DELIM" marker present in `line`, in
 * the same left-to-right order those markers appear in `line` (see
 * shell_command_t's own `heredoc_body` doc comment above, and
 * shell_app.c's shell__run_script(), the only current supplier of a non-empty
 * list) -- shell_parser__extract_redirect() hands out one entry per marker it
 * finds, by simple position, and reports "heredoc ... not supported here" if
 * it finds a marker with no corresponding entry left (an interactive line, a
 * loop/function body being re-parsed, ... -- see shell_parser.c). Pass
 * `NULL, 0` when `line` is known to carry no heredoc of its own. */
int shell_parser__plan(
    const char *line, shell_plan_t *plan, char *const *heredoc_bodies, size_t heredoc_count, const char **error
);
void shell_parser__plan_free(shell_plan_t *plan);

/* Scans `line` (`length` bytes, a single physical line -- not a joined
 * multi-line block) for a top-level "<<"/"<<-" heredoc-body marker, skipping
 * quoted text and "$(...)"/"`...`" spans the same way shell_parser__plan()'s
 * own scan does, so e.g. `echo "a << b"` is never mistaken for one. Used by
 * shell_app.c's shell__run_script() to recognize a heredoc *before* the line
 * is joined into a multi-line block, since the physical lines that follow
 * must be read as raw body text rather than fed back through ordinary
 * line-accumulation/parsing. Only the line's first top-level "<<" is ever
 * considered -- like every other redirection this shell supports, at most
 * one heredoc per command.
 *
 * Returns false with *error == NULL when no marker is present at all (not a
 * failure, just nothing to do here); returns false with *error set for one
 * that *is* present but malformed ("<<" with no delimiter word, an
 * unterminated quote in it, ...). On a real find, returns true and sets
 * *out_strip_tabs ("<<-": strip each body line's leading tabs, and the
 * terminator line's, before comparing), *out_literal (delimiter was
 * single- or double-quoted: suppress $expansion in the collected body), and
 * *out_delim / *out_delim_len (a span into `line` itself -- not
 * NUL-terminated, not unescaped/unquoted). */
bool shell_parser__find_heredoc_marker(
    const char *line, size_t length, bool *out_strip_tabs, bool *out_literal, const char **out_delim,
    size_t *out_delim_len, const char **error
);

/* Expands "$NAME"/"${NAME}"/"$?"/"$0".."$9"/"$#"/"$(...)"/"$((...))"
 * constructs within `text` (`length` bytes) exactly the way
 * shell_parser__words() expands one word's worth of them, except over an
 * arbitrary multi-line span -- real '\n' bytes in `text` are kept as literal
 * data, never treated as a word separator -- and capped at SHELL__HEREDOC_MAX
 * rather than SHELL__WORD_MAX. Used for an unquoted-delimiter heredoc body
 * (shell_app.c's shell__run_script()), which bash expands the same way a
 * double-quoted string's contents are: substitutions run, but the result is
 * never itself word-split or globbed. Backslash only keeps its special
 * meaning right before '$', '`', '\\', or a newline (the last one dropping
 * both bytes -- a line continuation); every other backslash, e.g. in "a\nb"
 * or "C:\path", is left completely alone, matching bash's own (narrower than
 * a double-quoted word's) heredoc-body escaping rule. Returns a
 * heap-allocated (memory__malloc(), caller-owned via memory__free()),
 * NUL-terminated string, or NULL on failure (an unterminated "$(...)"/
 * "$((...))", or the expanded result outgrowing SHELL__HEREDOC_MAX) with
 * *error set. */
char *shell_parser__expand_text(
    const char *text, size_t length, shell_variable_lookup_fn lookup, shell_command_substitution_fn substitute,
    shell_arith_word_fn arith, void *context, int last_status, const char **error
);

/* Tokenizes `command` into a heap-allocated, NULL-terminated argv-style array:
 * each word is allocated to its exact final length. `lookup`/`substitute`/
 * `arith` are called with the same `context` for $NAME/$(...)-or-`...`/
 * $((...))-style expansion respectively (see their typedefs above). On
 * success *out_words is non-NULL when *word_count > 0 and must be released
 * with shell_parser__free_words(); on failure (returns -1) *out_words is
 * NULL. */
int shell_parser__words(
    const shell_command_t *command, char ***out_words, int *word_count, shell_variable_lookup_fn lookup,
    shell_command_substitution_fn substitute, shell_arith_word_fn arith, void *context, int last_status,
    const char **error
);
void shell_parser__free_words(char **words, int word_count);

bool shell_parser__valid_name(const char *name, size_t length);
