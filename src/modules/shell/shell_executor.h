#pragma once

#include "shell_internal.h"
#include "shell_parser.h"

int shell_executor__plan(shell_state_t *state, const shell_plan_t *plan);
int shell_executor__page_help(void);

/* $0/$1../$9/$# and named-variable resolution for shell_parser__words()
 * (see shell_executor.c for exactly what this does) -- exported so
 * shell_compound.c's word-list `for NAME in WORD...` can expand its list
 * the same way an ordinary command's arguments are expanded. */
const char *shell_executor__lookup(void *context, const char *name);

/* Runs "$(...)" / "`...`" command substitution for shell_parser__words()
 * (see shell_executor.c for exactly what this does) -- exported so
 * shell_compound.c's word-list `for NAME in WORD...` can expand its list
 * the same way an ordinary command's arguments are expanded. */
char *shell_executor__run_substitution(void *context, const char *command_text, size_t length);

/* Evaluates "$((...))" arithmetic-expansion words for shell_parser__words()
 * (see shell_executor.c for exactly what this does) -- exported so
 * shell_compound.c's word-list `for NAME in WORD...` can expand its list
 * the same way an ordinary command's arguments are expanded. */
char *shell_executor__eval_arith_word(void *context, const char *text, size_t length, const char **error);
