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
