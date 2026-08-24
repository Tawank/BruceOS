#pragma once

/* Interprets the subset of shell grammar that spans more than one flat
 * command: `if`/`elif`/`else`/`fi` and `name() { ...; }` function
 * definitions. Everything else (a plain command, or a run of them joined by
 * ;/&&/||/|) is delegated straight back to shell_executor__plan(), which is
 * still what actually runs a simple command. Not part of the public
 * core_sdk/ API. */

#include <stdbool.h>
#include <stddef.h>

#include "shell_internal.h"

/* Parses and runs `text` -- one logical line, or a whole multi-line
 * if/fi or function block already assembled by the caller (see
 * shell_compound__pending() below). This is what shell__execute_line() and
 * a function call's body both run through. */
int shell_compound__run(shell_state_t *state, const char *text);

/* True if `text` (everything accumulated so far, physical lines joined by a
 * real '\n') is not yet a complete, well-formed unit -- an open quote, an
 * "if" with no matching "fi" yet, or a "{" with no matching "}" yet -- so
 * the caller (shell_app.c's script/interactive readers) should keep
 * appending lines instead of running it. */
bool shell_compound__pending(const char *text);

bool shell_compound__is_function(const shell_state_t *state, const char *name);

/* Calls the function named `name` (already confirmed defined via
 * shell_compound__is_function()) with argv[1..argc) bound to $1.. for the
 * duration of the call; argv[0] is the function name itself ($0). Returns
 * 127 if `name` isn't actually a defined function. */
int shell_compound__call_function(shell_state_t *state, int argc, char **argv);

/* Releases the function table owned by `state` -- pairs with
 * shell__state_free() in shell_app.c the same way shell_builtins__set()'s
 * variable table is released there directly. */
void shell_compound__state_free(shell_state_t *state);
