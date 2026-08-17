#pragma once

/* Shared state between shell_app.c/shell_executor.c/shell_builtins.c and the
 * selftest module. Not part of the public core_sdk/ API: other modules must
 * not include this header, only shell_app.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Safety caps against runaway scripts, not preallocation sizes: variables and
 * words are heap-allocated to their actual length. */
#define SHELL__MAX_VARIABLES 24
#define SHELL__VARIABLE_NAME_MAX 32
#define SHELL__VARIABLE_VALUE_MAX 128
#define SHELL__LINE_MAX 512

typedef struct {
    char *name;
    char *value;
    bool exported;
} shell_variable_t;

typedef struct {
    shell_variable_t *variables;
    size_t variable_count;
    size_t variable_capacity;
    int last_status;
    bool exit_requested;
    int exit_status;
    /* Last tty__get_size() generation applied to COLUMNS/LINES -- see
     * shell__sync_tty_size in shell_app.c. Starts at 0 (calloc'd), which
     * never equals a real session's generation (tty__set_size always bumps
     * it to at least 1 on its first call), so the initial size is picked up
     * the first time it's checked. */
    uint32_t tty_generation;
} shell_state_t;

void shell__state_init(shell_state_t *state);
void shell__state_free(shell_state_t *state);
int shell__execute_line(shell_state_t *state, const char *line);
