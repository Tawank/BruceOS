#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SHELL__MAX_COMMANDS 32
#define SHELL__MAX_WORDS 24
#define SHELL__WORD_MAX 256
#define SHELL__PARSED_NAME_MAX 32

typedef enum {
    SHELL_CONNECT_NONE,
    SHELL_CONNECT_SEQUENCE,
    SHELL_CONNECT_AND,
    SHELL_CONNECT_OR,
} shell_connector_t;

typedef struct {
    const char *text;
    size_t length;
    shell_connector_t connector;
} shell_command_t;

typedef struct {
    shell_command_t commands[SHELL__MAX_COMMANDS];
    size_t count;
} shell_plan_t;

typedef const char *(*shell_variable_lookup_fn)(void *context, const char *name);

int shell_parser__plan(const char *line, shell_plan_t *plan, const char **error);
int shell_parser__words(
    const shell_command_t *command, char words[SHELL__MAX_WORDS][SHELL__WORD_MAX], int *word_count,
    shell_variable_lookup_fn lookup, void *lookup_context, int last_status, const char **error
);
bool shell_parser__valid_name(const char *name, size_t length);
