#pragma once

#include "core_sdk/process.h"
#include "core_sdk/stdio.h"
#include "shell_internal.h"
#include "shell_parser.h"

int shell_executor__plan(shell_state_t *state, const shell_plan_t *plan);
int shell_executor__page_help(void);

/* Maps a finished process's status to the shell exit-code convention this
 * module uses everywhere a waited-for process's outcome becomes a shell
 * status: a normal exit keeps its low byte, a signalled
 * (terminated/killed) process reports 128+signal. Exported so shell_jobs.c's
 * "wait" builtin reports a backgrounded job's real exit status the same way
 * an ordinary (non-backgrounded) external command's already does. */
int shell_executor__status_to_exit_code(const bruce_process_status_t *status);

/* Blocks for `child` to finish, relaying this shell's own INT/TERM (Ctrl+C,
 * or a real "kill"/"terminate" aimed at the shell itself -- see
 * process__wait_status()'s BRUCE_ERR_CANCELLED) on to it instead of just
 * returning, the same way any other foreground external command already
 * gets Ctrl+C forwarded (see the README's "Ctrl+C" section). Returns the
 * real exit status via shell_executor__status_to_exit_code(). Exported so
 * shell_jobs.c's "fg" builtin can bring a background job into the same
 * foreground-wait treatment an ordinary (non-backgrounded) command gets. */
int shell_executor__wait(bruce_process_id_t child);

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

/* Sets up a stdio session carrying `command`'s own redirected stdin (a
 * "< file", "<<DELIM" heredoc, or "<<<WORD" here-string -- whichever of
 * `command`'s own fields is set, same as an ordinary single command's
 * stdin already works via shell_executor__load_redirect_input()) and routes
 * this process's own reads to it (stdio__session_capture_self()) --
 * exported so shell_compound.c's run_while()/run_for() can attach a
 * redirect found on "done" to the *entire* loop (every iteration's
 * condition and body, not just one command), the same way bash itself
 * lets "while ...; do ...; done <<< \"$t\"" feed every "read" inside the
 * loop from one shared source. Caller must pair a successful call with
 * shell_executor__teardown_redirected_stdin() once the loop is done, in
 * every exit path (natural completion or a mid-loop break alike). Returns
 * BRUCE_OK with *out_session set on success; on failure, an error has
 * already been printed and *out_session is left at
 * BRUCE_STDIO_SESSION_INVALID -- the caller should treat that as the whole
 * statement failing (matching shell_executor__dispatch()'s own convention
 * for a single command's bad redirect). */
bruce_result_t shell_executor__setup_redirected_stdin(
    shell_state_t *state, const shell_command_t *command, bruce_stdio_session_t *out_session
);

/* Undoes shell_executor__setup_redirected_stdin() -- releases this
 * process's own read-routing back to whatever it was before, and closes
 * `session`. A no-op if `session` is BRUCE_STDIO_SESSION_INVALID (the setup
 * call never having been made, or having failed), so callers can pair the
 * two unconditionally without their own extra check. */
void shell_executor__teardown_redirected_stdin(bruce_stdio_session_t session);
