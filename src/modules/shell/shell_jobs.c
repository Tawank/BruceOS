#include "shell_jobs.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "shell_executor.h"

static void shell_jobs__remove_at(shell_state_t *state, size_t index) {
    for (size_t i = index; i + 1 < state->job_count; ++i) state->jobs[i] = state->jobs[i + 1];
    state->job_count--;
}

int shell_jobs__add(shell_state_t *state, bruce_process_id_t pid, const char *display_text, size_t display_length) {
    if (state->job_count >= SHELL__MAX_JOBS) return 0;
    shell_job_t *job = &state->jobs[state->job_count++];
    job->number = ++state->next_job_number;
    job->pid = pid;
    size_t copy_length = display_length < sizeof(job->command) - 1u ? display_length : sizeof(job->command) - 1u;
    memcpy(job->command, display_text, copy_length);
    job->command[copy_length] = '\0';
    state->last_background_pid = pid;
    return job->number;
}

void shell_jobs__poll(shell_state_t *state) {
    for (size_t i = 0; i < state->job_count;) {
        bruce_process_status_t status;
        if (process__wait_status(state->jobs[i].pid, 0, &status) != BRUCE_OK) {
            ++i;
            continue;
        }
        if (status.reason == BRUCE_PROCESS_TERMINATED || status.reason == BRUCE_PROCESS_KILLED) {
            stdio__printf("[%d]+ Killed\t%s\n", state->jobs[i].number, state->jobs[i].command);
        } else if (status.exit_code != 0) {
            stdio__printf("[%d]+ Exit %d\t%s\n", state->jobs[i].number, status.exit_code, state->jobs[i].command);
        } else {
            stdio__printf("[%d]+ Done\t%s\n", state->jobs[i].number, state->jobs[i].command);
        }
        /* Don't advance `i`: shell_jobs__remove_at() shifted the next entry
         * (if any) down into this slot. */
        shell_jobs__remove_at(state, i);
    }
}

int shell_jobs__run(shell_state_t *state, int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < state->job_count; ++i) {
        stdio__printf("[%d]  Running\t%s\n", state->jobs[i].number, state->jobs[i].command);
    }
    return 0;
}

/* Parses "%N" (job number) or a bare decimal PID against the tracked table;
 * returns the table index, or (size_t)-1 if not found. */
static size_t shell_jobs__find(const shell_state_t *state, const char *token) {
    if (token[0] == '%') {
        char *end = NULL;
        long number = strtol(token + 1, &end, 10);
        if (end == token + 1 || *end != '\0') return (size_t)-1;
        for (size_t i = 0; i < state->job_count; ++i) {
            if (state->jobs[i].number == (int)number) return i;
        }
        return (size_t)-1;
    }
    char *end = NULL;
    unsigned long pid = strtoul(token, &end, 10);
    if (end == token || *end != '\0') return (size_t)-1;
    for (size_t i = 0; i < state->job_count; ++i) {
        if (state->jobs[i].pid == (bruce_process_id_t)pid) return i;
    }
    return (size_t)-1;
}

int shell_jobs__wait(shell_state_t *state, int argc, char **argv) {
    if (argc <= 1) {
        while (state->job_count > 0) {
            bruce_process_status_t status;
            /* A failed wait (e.g. Ctrl+C interrupting this blocking call --
             * see process__wait_status()'s BRUCE_ERR_CANCELLED) stops here
             * without touching the table: whatever hasn't finished yet stays
             * tracked, the same way bash's own "wait" leaves unfinished jobs
             * alone when interrupted. */
            if (process__wait_status(state->jobs[0].pid, UINT32_MAX, &status) != BRUCE_OK) break;
            shell_jobs__remove_at(state, 0);
        }
        return 0;
    }
    size_t index = shell_jobs__find(state, argv[1]);
    if (index == (size_t)-1) {
        stdio__printf("shell: wait: %s: no such job\n", argv[1]);
        return 1;
    }
    bruce_process_status_t status;
    if (process__wait_status(state->jobs[index].pid, UINT32_MAX, &status) != BRUCE_OK) return 1;
    /* Re-resolve rather than trusting the `index` captured above: nothing
     * else can touch state->jobs while this call is blocked waiting (it's
     * the shell's own single task), but doing it this way costs nothing and
     * doesn't rely on that staying true. */
    index = shell_jobs__find(state, argv[1]);
    if (index != (size_t)-1) shell_jobs__remove_at(state, index);
    return shell_executor__status_to_exit_code(&status);
}

/* Parses a kill signal spec -- "-s SIGNAL"'s value, or a bare "-N"/"-NAME"
 * token found among shell_jobs__kill()'s arguments -- accepting a signal
 * number (2/9/15), a bare name (int/term/kill), or a name with the usual
 * "SIG" prefix, all case-insensitively. A leading '-' is stripped first so
 * this handles both "-s KILL" (already just "KILL" by the time it gets
 * here) and the raw "-KILL"/"-9" token the same way. */
static bool shell_jobs__parse_signal(const char *text, bruce_process_signal_t *out_signal) {
    if (text == NULL || text[0] == '\0') return false;
    if (text[0] == '-') text++;
    if (text[0] == '\0') return false;
    const char *name = text;
    if (strncasecmp(name, "SIG", 3) == 0 && name[3] != '\0') name += 3;
    if (strcasecmp(name, "INT") == 0) {
        *out_signal = BRUCE_PROCESS_SIGNAL_INT;
        return true;
    }
    if (strcasecmp(name, "TERM") == 0) {
        *out_signal = BRUCE_PROCESS_SIGNAL_TERM;
        return true;
    }
    if (strcasecmp(name, "KILL") == 0) {
        *out_signal = BRUCE_PROCESS_SIGNAL_KILL;
        return true;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0') return false;
    if (value != BRUCE_PROCESS_SIGNAL_INT && value != BRUCE_PROCESS_SIGNAL_TERM &&
        value != BRUCE_PROCESS_SIGNAL_KILL) {
        return false;
    }
    *out_signal = (bruce_process_signal_t)value;
    return true;
}

/* True for a token that names a signal rather than a target -- a lone '-'
 * followed by digits or letters and nothing else (bash's "-SIGSPEC" kill
 * syntax, e.g. "-9"/"-KILL"/"-TERM"). Neither a "%N" job spec nor a raw PID
 * can start with '-', so this can't misclassify either. */
static bool shell_jobs__looks_like_signal_spec(const char *text) {
    if (text == NULL || text[0] != '-' || text[1] == '\0') return false;
    for (const char *p = text + 1; *p != '\0'; ++p) {
        if (!isalnum((unsigned char)*p)) return false;
    }
    return true;
}

/* Resolves a kill target to a PID: "%N" must name a tracked job (there is no
 * other way to learn its PID -- see shell_jobs__find()); a bare number is
 * used as-is whether or not it happens to also be a tracked job, matching
 * real kill(1) (unlike "wait", it isn't limited to jobs this shell knows
 * about). */
static bool shell_jobs__resolve_kill_target(const shell_state_t *state, const char *token, bruce_process_id_t *out_pid) {
    if (token[0] == '%') {
        size_t index = shell_jobs__find(state, token);
        if (index == (size_t)-1) return false;
        *out_pid = state->jobs[index].pid;
        return true;
    }
    char *end = NULL;
    unsigned long pid = strtoul(token, &end, 10);
    if (end == token || *end != '\0' || pid == 0) return false;
    *out_pid = (bruce_process_id_t)pid;
    return true;
}

int shell_jobs__kill(shell_state_t *state, int argc, char **argv) {
    bruce_process_signal_t signal = BRUCE_PROCESS_SIGNAL_TERM;
    int first_target = 1;
    if (argc >= 3 && strcmp(argv[1], "-s") == 0) {
        if (!shell_jobs__parse_signal(argv[2], &signal)) {
            stdio__printf("shell: kill: %s: invalid signal\n", argv[2]);
            return 1;
        }
        first_target = 3;
    }
    /* A "-SIGSPEC" token can appear anywhere among the remaining arguments
     * (bash doesn't require it first) -- resolved in its own pass so it
     * applies to every target regardless of where it showed up relative to
     * them. */
    for (int i = first_target; i < argc; ++i) {
        if (!shell_jobs__looks_like_signal_spec(argv[i])) continue;
        if (!shell_jobs__parse_signal(argv[i], &signal)) {
            stdio__printf("shell: kill: %s: invalid signal\n", argv[i]);
            return 1;
        }
    }

    int status = 0;
    int target_count = 0;
    for (int i = first_target; i < argc; ++i) {
        if (shell_jobs__looks_like_signal_spec(argv[i])) continue;
        target_count++;
        bruce_process_id_t pid = BRUCE_PROCESS_ID_INVALID;
        if (!shell_jobs__resolve_kill_target(state, argv[i], &pid)) {
            stdio__printf("shell: kill: %s: no such job or process\n", argv[i]);
            status = 1;
            continue;
        }
        bruce_result_t result = process__signal(pid, signal);
        if (result != BRUCE_OK) {
            stdio__printf("shell: kill: (%s): %s\n", argv[i], result__to_string(result));
            status = 1;
        }
    }
    if (target_count == 0) {
        stdio__printf("shell: kill: usage: kill [-s SIGNAL] PID|%%JOB ...\n");
        return 1;
    }
    return status;
}
