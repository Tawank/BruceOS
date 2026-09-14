#pragma once

#include <stdbool.h>

#include "shell_internal.h"

bool shell_builtins__is_builtin(const char *name);
size_t shell_builtins__count(void);
const char *shell_builtins__name(size_t index);

/* One-line description of the builtin at `index` (same indexing as
 * shell_builtins__name()), for `man`'s "Shell Built-ins" listing and its
 * --gen-md section (see man_app.c) -- shell_executor.c's own "shell help"
 * listing only ever needed the bare name. Returns NULL for an
 * out-of-range index. */
const char *shell_builtins__description(size_t index);
int shell_builtins__run(shell_state_t *state, int argc, char **argv);
const char *shell_builtins__get(const shell_state_t *state, const char *name);
int shell_builtins__set(shell_state_t *state, const char *name, const char *value);
int shell_builtins__export(shell_state_t *state, const char *name);

/* Removes `name` entirely (a no-op if it isn't set) and, if it was exported,
 * drops it from the process environment too -- the guts of the `unset`
 * builtin, exported so shell_compound__call_function() can reuse it to tear
 * a `local` frame back down once a call returns (see shell_local_frame_t in
 * shell_internal.h). `name` is assumed already validated by the caller. */
void shell_builtins__unset(shell_state_t *state, const char *name);

/* Resolves `path` (NULL or "" means the current directory itself) against
 * $PWD into an absolute, "."/".."-normalized path the storage SDK will
 * accept -- the same resolution `cd`'s own argument goes through. Exported
 * so shell_executor.c's ">"/">>" redirection targets behave like any other
 * shell path argument (relative to $PWD) rather than requiring a full
 * absolute path. out_path must have room for BRUCE_STORAGE_PATH_MAX bytes;
 * returns false (leaving out_path's contents unspecified) if the resolved
 * path would not fit. */
bool shell_builtins__resolve_path(const shell_state_t *state, const char *path, char *out_path);

/* Runs this shell_state_t's EXIT trap (state->trap_exit), if any, exactly
 * once -- see state->trap_exit_fired's doc comment in shell_internal.h.
 * Called from every point in shell_app.c a shell/script run actually ends
 * (end of shell__run_script(), end of shell__interactive(), and a "-c"
 * command's own line in shell_app_main()), right before that point computes
 * its final exit status, so a trap action that itself calls `exit M`
 * overrides it -- matching bash. A no-op if no EXIT trap is set. */
void shell_builtins__fire_exit_trap(shell_state_t *state);

/* Checks whether `signal` (BRUCE_PROCESS_SIGNAL_INT/_TERM only -- KILL isn't
 * catchable, same as real kill(2)/trap) has a trap set (state->trap_int/
 * trap_term); if so, clears the live signal and runs the trap action (a ""
 * action, from `trap '' SIGSPEC`, means "ignore" -- the signal is still
 * cleared, just nothing runs), returning true either way so the caller
 * skips its own default handling for this signal. Returns false for a
 * signal with no trap set (the caller should fall back to its own default
 * behavior) or one this function doesn't own at all.
 *
 * Only ever consulted from the handful of points this shell already polls
 * process__current_signal() at (shell_app.c's idle prompt,
 * shell_compound__loop_should_stop()) -- a signal delivered while a
 * foreground external command is running synchronously outside any loop
 * isn't seen here until the next such poll, since shell_executor__wait()
 * has no shell_state_t to consult a trap through. */
bool shell_builtins__fire_signal_trap(shell_state_t *state, bruce_process_signal_t signal);
