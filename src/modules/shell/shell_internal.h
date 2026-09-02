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
/* A multi-line construct (if/fi, a function body) accumulates several
 * physical lines before it runs -- see shell_compound__pending() and its
 * callers in shell_app.c. This bounds that accumulation buffer, same
 * "runaway input" role SHELL__LINE_MAX plays for a single line. */
#define SHELL__BLOCK_MAX 4096
#define SHELL__MAX_FUNCTIONS 8
#define SHELL__FUNCTION_NAME_MAX SHELL__VARIABLE_NAME_MAX
#define SHELL__FUNCTION_BODY_MAX SHELL__BLOCK_MAX
/* $1..$9: bash only ever expands a single digit unbraced ($10 is $1 followed
 * by a literal "0"), so this is a hard grammar limit, not a tunable cap. */
#define SHELL__MAX_POSITIONAL 9

typedef struct {
    char *name;
    char *value;
    bool exported;
} shell_variable_t;

typedef struct {
    char *name;
    char *body;
} shell_function_t;

/* One name the `local` builtin has shadowed during the function call that
 * owns this entry's frame (see shell_local_frame_t below), recording what to
 * put back once that call returns: `previous_value` is a copy of what
 * shell_builtins__get() returned for `name` right before it was first
 * localized this call, or NULL if `name` didn't exist at all yet (in which
 * case the call unsets it on the way out instead of restoring a value). */
typedef struct {
    char *name;
    char *previous_value;
} shell_local_entry_t;

/* One function call's `local` bookkeeping -- shell_compound__call_function()
 * stack-allocates one of these per call (the same "lives on the C call
 * stack, saved/restored around the call" shape it already uses for
 * state->positional) and points state->local_frame at it for the call's
 * duration, so shell_builtins__local() always records into the innermost
 * active call regardless of how deep the recursion goes. */
typedef struct {
    shell_local_entry_t *entries;
    size_t count;
    size_t capacity;
} shell_local_frame_t;

typedef struct {
    shell_variable_t *variables;
    size_t variable_count;
    size_t variable_capacity;
    shell_function_t *functions;
    size_t function_count;
    size_t function_capacity;
    /* The currently-executing function call's `local` frame, or NULL outside
     * any function call -- see shell_local_frame_t above. Owned by whichever
     * shell_compound__call_function() stack frame is innermost; never
     * allocated or freed through this pointer itself. */
    shell_local_frame_t *local_frame;
    /* $0/$1../$#/$@ for the function call currently executing (see
     * shell_compound.c); empty/NULL at top level, outside any call. */
    const char *arg0;
    char **positional;
    int positional_count;
    /* Scratch space $#'s expansion is formatted into by
     * shell_executor__lookup() -- see shell_parser__expand()'s digit branch
     * in shell_parser.c, which is what actually asks for "#". */
    char positional_count_text[4];
    int last_status;
    bool exit_requested;
    int exit_status;
    /* Set by the `break` builtin to the number of enclosing for/while loops
     * still left to unwind (N for "break N", 1 for a bare "break"). Checked
     * the same way exit_requested is: shell_compound__run_sequence() stops
     * running further statements while it's nonzero, and each enclosing
     * loop (shell_compound__run_for()/run_while()) decrements it by one and
     * stops iterating when it does so, so it naturally reaches 0 at the
     * right nesting level. A stray "break" outside any loop -- or one that
     * outlives every loop it could apply to -- is caught and reset back to
     * 0 by shell_compound__run(), the same function-call/top-level-line
     * boundary a real bash restricts break's effect to. */
    int break_requested;
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
