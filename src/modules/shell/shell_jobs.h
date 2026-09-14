#pragma once

/* "cmd &" / "func &" background-job bookkeeping: the small table of
 * currently-tracked jobs on shell_state_t (see shell_internal.h's
 * shell_job_t), the "jobs"/"wait" builtins, and $! resolution. Every job
 * here was already launched as its own process (see
 * shell_executor__external_background()/shell_executor__function_background()
 * in shell_executor.c) via the same process-registry path an ordinary,
 * waited-for external command uses -- this module only adds the "don't wait,
 * remember it, let the user check on it later" layer on top. Not part of the
 * public core_sdk/ API. */

#include <stddef.h>

#include "core_sdk/process.h"
#include "shell_internal.h"

/* Records a newly-launched, not-waited-for job: truncates `display_text`
 * (`display_length` bytes, not necessarily NUL-terminated) into the job's
 * fixed-size display buffer, sets $!'s value to `pid`, and returns the
 * assigned 1-based job number -- or 0 if the table is already full
 * (SHELL__MAX_JOBS), in which case the caller falls back to waiting for
 * `pid` synchronously instead of leaving it untracked. */
int shell_jobs__add(shell_state_t *state, bruce_process_id_t pid, const char *display_text, size_t display_length);

/* Non-blocking: reaps and reports every tracked job that has finished since
 * the last call ("[N]+ Done\t<command>" / "Exit <code>" / "Killed"), same
 * wording bash itself uses. Called once per prompt from shell_app.c's
 * shell__interactive() so completions surface right before the next prompt,
 * the same timing bash uses. */
void shell_jobs__poll(shell_state_t *state);

/* The "jobs" builtin: lists every still-tracked job (anything already
 * finished was already reaped by the last shell_jobs__poll()). Always
 * returns 0. */
int shell_jobs__run(shell_state_t *state, int argc, char **argv);

/* The "wait" builtin. Bare "wait": blocks for every tracked job to finish (in
 * job-number order), reaping each, and returns 0. "wait %N" or "wait PID":
 * blocks for just that one job, reaps it, and returns its real exit status
 * (see shell_executor__status_to_exit_code()); reports "no such job" and
 * returns 1 if `argv[1]` doesn't name a tracked job. */
int shell_jobs__wait(shell_state_t *state, int argc, char **argv);

/* The "kill" builtin. Like bash's own "kill" (also a shell builtin, not an
 * external program -- precisely so it can resolve "%N" job specs against
 * this shell's own job table), sends a signal to one or more targets, each
 * either "%N" (a tracked job -- see shell_jobs__run() above for what "jobs"
 * would print) or a raw PID (any live process, tracked or not, matching
 * real kill(1)'s own reach). Accepts "-s SIGNAL" or a bare "-SIGSPEC" token
 * (name or number, case-insensitive, with or without the usual "SIG"
 * prefix; TERM if neither is given, bash's own default) anywhere among the
 * arguments. Reports and continues past a bad target instead of stopping at
 * the first one, returning 0 only if every target was resolved and
 * signaled; "kill" with no targets at all reports usage and returns 1. */
int shell_jobs__kill(shell_state_t *state, int argc, char **argv);

/* The "fg" builtin: brings a background job into the foreground -- prints its
 * command line (same text "jobs" would show), then blocks for it via
 * shell_executor__wait() so it gets the same Ctrl+C relay a plain foreground
 * command already does, and reports its real exit status. "fg %N"/"fg PID"
 * targets that job; a bare "fg" targets the most recently backgrounded job
 * (state->jobs[job_count - 1]) -- there's no bash-style "%+"/"%-"
 * current/previous marker here, just the table's own append order. Reports
 * "no current job"/"no such job" and returns 1 if there's nothing to bring
 * up. */
int shell_jobs__fg(shell_state_t *state, int argc, char **argv);

/* The "bg" builtin: resumes a paused job in the background (the
 * process__pause()/process__resume() this shell never itself triggers --
 * see shell_jobs__bg()'s own doc comment in shell_jobs.c for how a job could
 * still end up paused). Same target resolution and "most recent job" default
 * as "fg" above, but never blocks: reports "already in background" and
 * returns 1 if the job isn't actually paused. */
int shell_jobs__bg(shell_state_t *state, int argc, char **argv);

/* The "disown" builtin: forgets a tracked job without touching the process
 * itself -- it keeps running, but "jobs"/"wait"/the next prompt's completion
 * message no longer know about it (same table removal shell_jobs__remove_at()
 * already does for a job "wait" finishes reaping, just without ever having
 * waited). Same target resolution and "most recent job" default as "fg"/"bg".
 * Always returns 0 once a job is found; "no current job"/"no such job"
 * returns 1. */
int shell_jobs__disown(shell_state_t *state, int argc, char **argv);
