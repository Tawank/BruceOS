#include "shell_jobs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
