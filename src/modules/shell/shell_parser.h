#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SHELL__MAX_COMMANDS 32
/* Safety caps against runaway expansions, not preallocation sizes. */
#define SHELL__MAX_WORDS 24
#define SHELL__WORD_MAX 256
#define SHELL__PARSED_NAME_MAX 32

typedef enum {
    SHELL_CONNECT_NONE,
    SHELL_CONNECT_SEQUENCE,
    SHELL_CONNECT_AND,
    SHELL_CONNECT_OR,
    SHELL_CONNECT_PIPE,
} shell_connector_t;

/* A single trailing ">"/">>" output redirection recognized on one command by
 * shell_parser__plan() -- see its shell_parser__extract_redirect() helper.
 * There is no SHELL_REDIRECT_IN: input redirection ('<') is rejected at
 * parse time (see shell_executor.c's README notes for why -- this shell's
 * stdio sessions have no generic EOF signal a plain external command could
 * detect). */
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

int shell_parser__plan(const char *line, shell_plan_t *plan, const char **error);
void shell_parser__plan_free(shell_plan_t *plan);

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
