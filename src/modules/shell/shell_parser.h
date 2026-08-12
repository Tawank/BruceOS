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

typedef struct {
    const char *text;
    size_t length;
    shell_connector_t connector;
} shell_command_t;

typedef struct {
    shell_command_t *commands;
    size_t count;
    size_t capacity;
} shell_plan_t;

typedef const char *(*shell_variable_lookup_fn)(void *context, const char *name);

int shell_parser__plan(const char *line, shell_plan_t *plan, const char **error);
void shell_parser__plan_free(shell_plan_t *plan);

/* Tokenizes `command` into a heap-allocated, NULL-terminated argv-style array:
 * each word is allocated to its exact final length. On success *out_words is
 * non-NULL when *word_count > 0 and must be released with
 * shell_parser__free_words(); on failure (returns -1) *out_words is NULL. */
int shell_parser__words(
    const shell_command_t *command, char ***out_words, int *word_count, shell_variable_lookup_fn lookup,
    void *lookup_context, int last_status, const char **error
);
void shell_parser__free_words(char **words, int word_count);

bool shell_parser__valid_name(const char *name, size_t length);
