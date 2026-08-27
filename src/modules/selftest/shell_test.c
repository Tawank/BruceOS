#include "shell_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"
#include "modules/shell/shell_app.h"
#include "modules/shell/shell_internal.h"

static volatile int s_probe_calls;
static char s_probe_arg[64];
static char s_probe_environment[64];
static char s_probe_gui[4];
static char s_probe_pwd[BRUCE_STORAGE_PATH_MAX];

static int selftest__shell_probe(int argc, char **argv) {
    s_probe_calls++;
    snprintf(s_probe_arg, sizeof(s_probe_arg), "%s", argc > 1 ? argv[1] : "");
    const char *exported = environment__get("EXPORTED");
    const char *temporary = environment__get("TEMPORARY");
    snprintf(
        s_probe_environment, sizeof(s_probe_environment), "%s|%s",
        exported != NULL ? exported : "", temporary != NULL ? temporary : ""
    );
    const char *gui = environment__get("GUI");
    snprintf(s_probe_gui, sizeof(s_probe_gui), "%s", gui != NULL ? gui : "");
    const char *pwd = environment__get("PWD");
    snprintf(s_probe_pwd, sizeof(s_probe_pwd), "%s", pwd != NULL ? pwd : "");
    if (argc > 1 && strcmp(argv[1], "nonzero") == 0) return 37;
    if (argc > 1 && strcmp(argv[1], "routed") == 0) stdio__printf("shell-grandchild-routed\n");
    return argc > 2 && strcmp(argv[1], argv[2]) == 0 ? 0 : (argc > 2 ? 1 : 0);
}

static bool selftest__shell_register_probe(void) {
    bruce_result_t result =
        app_runner__register("shell_test_probe", "Shell integration probe", "Test", selftest__shell_probe, 0);
    return result == BRUCE_OK || result == BRUCE_ERR_ALREADY_EXISTS;
}

bool selftest__run_shell_language_case(void) {
    if (!selftest__shell_register_probe()) return false;
    if (environment__set("INHERITED_SHELL", "visible") != BRUCE_OK) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));
    memset(s_probe_gui, 0, sizeof(s_probe_gui));
    memset(s_probe_pwd, 0, sizeof(s_probe_pwd));

    bool ok =
        shell__execute_line(&state, "shell_test_probe 'a b' \"a b\"") == 0 &&
        strcmp(s_probe_arg, "a b") == 0 &&
        strcmp(s_probe_gui, "0") == 0 &&
        shell__execute_line(&state, "GUI=1 shell_test_probe gui") == 0 &&
        strcmp(s_probe_gui, "1") == 0 &&
        shell__execute_line(&state, "shell_test_probe escaped\\ word 'escaped word'") == 0 &&
        shell__execute_line(&state, "VALUE=stored; shell_test_probe $VALUE stored") == 0 &&
        shell__execute_line(&state, "false; shell_test_probe $? 1") == 0 &&
        shell__execute_line(&state, "true || shell_test_probe skipped; false && shell_test_probe skipped") ==
            1 &&
        s_probe_calls == 5 && shell__execute_line(&state, "false || true && false") == 1 &&
        shell__execute_line(&state, "echo ok; true") == 0 &&
        shell__execute_line(&state, "LOCAL_ONLY=yes; shell_test_probe env") == 0 &&
        strcmp(s_probe_environment, "|") == 0 &&
        shell__execute_line(&state, "export EXPORTED=yes; shell_test_probe env") == 0 &&
        strcmp(s_probe_environment, "yes|") == 0 &&
        shell__execute_line(&state, "TEMPORARY=once shell_test_probe env") == 0 &&
        strcmp(s_probe_environment, "yes|once") == 0 &&
        shell__execute_line(&state, "unset EXPORTED; shell_test_probe env") == 0 &&
        strcmp(s_probe_environment, "|") == 0 &&
        shell__execute_line(&state, "cd /apps; shell_test_probe $PWD /apps") == 0 &&
        strcmp(s_probe_pwd, "/apps") == 0 &&
        shell__execute_line(&state, "cd ..; shell_test_probe $PWD /") == 0 &&
        strcmp(s_probe_pwd, "/") == 0 &&
        shell__execute_line(&state, "shell_test_probe nonzero") == 37 &&
        shell__execute_line(&state, "echo broken | echo nope") == 2 &&
        shell__execute_line(&state, "echo > file") == 2 &&
        shell__execute_line(&state, "echo 'unterminated") == 2;
    if (ok) ok = shell__execute_line(&state, "shell_test_probe $INHERITED_SHELL visible") == 0;
    shell__state_free(&state);
    (void)environment__unset("INHERITED_SHELL");
    printf("[selftest] shell/language: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_script_case(void) {
    if (!selftest__shell_register_probe()) return false;
    const char *path = "/apps/shell_state_test.sh";
    const char script[] =
        "SCRIPT_VALUE=across-lines\n# retained state\nshell_test_probe $SCRIPT_VALUE across-lines\n";
    (void)storage__remove(path);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    if (storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, script, sizeof(script) - 1, &written) != BRUCE_OK ||
        written != sizeof(script) - 1 || storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(path);
        return false;
    }
    char *argv[] = {"shell", (char *)path, NULL};
    int status = shell_app_main(2, argv);
    (void)storage__remove(path);
    bool ok = status == 0 && strcmp(s_probe_arg, "across-lines") == 0;
    printf("[selftest] shell/script-state: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_control_flow_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok =
        /* if/then/fi, if/then/else/fi, if/elif/else/fi -- each only fires
         * its taken branch, and the untaken branches never call the probe. */
        shell__execute_line(&state, "if true; then shell_test_probe then_ran; fi") == 0 &&
        strcmp(s_probe_arg, "then_ran") == 0 && s_probe_calls == 1 &&
        shell__execute_line(&state, "if false; then shell_test_probe skipped; fi") == 0 && s_probe_calls == 1 &&
        shell__execute_line(&state, "if false; then shell_test_probe skipped; else shell_test_probe else_ran; fi") ==
            0 &&
        strcmp(s_probe_arg, "else_ran") == 0 && s_probe_calls == 2 &&
        shell__execute_line(
            &state,
            "if false; then shell_test_probe skipped; elif true; then shell_test_probe elif_ran; else "
            "shell_test_probe skipped; fi"
        ) == 0 &&
        strcmp(s_probe_arg, "elif_ran") == 0 && s_probe_calls == 3 &&
        /* No branch taken and no else: exit status is 0, same as bash. */
        shell__execute_line(&state, "if false; then shell_test_probe skipped; fi") == 0 && s_probe_calls == 3 &&
        /* A condition list can itself use ; and &&/||. */
        shell__execute_line(&state, "if true; false; then shell_test_probe skipped; fi") == 0 &&
        s_probe_calls == 3 &&
        /* test/[/[[ builtins: numeric, string, -z/-n, and -a. */
        shell__execute_line(&state, "[ 1 -eq 1 ] && shell_test_probe num_eq") == 0 &&
        strcmp(s_probe_arg, "num_eq") == 0 &&
        shell__execute_line(&state, "[ 1 -eq 2 ] && shell_test_probe skipped") == 1 &&
        shell__execute_line(&state, "test 3 -ne 2 -a 3 -gt 1 && shell_test_probe test_a") == 0 &&
        strcmp(s_probe_arg, "test_a") == 0 &&
        shell__execute_line(&state, "[[ 5 -ge 5 ]] && shell_test_probe dbracket") == 0 &&
        strcmp(s_probe_arg, "dbracket") == 0 &&
        shell__execute_line(&state, "[ a = a ] && shell_test_probe streq") == 0 &&
        shell__execute_line(&state, "[ a != b ] && shell_test_probe strneq") == 0 &&
        shell__execute_line(&state, "[ -z \"\" ] && shell_test_probe zempty") == 0 &&
        shell__execute_line(&state, "[ -n x ] && shell_test_probe nnonempty") == 0 &&
        shell__execute_line(&state, "[ x && shell_test_probe missing_bracket") == 2 &&
        shell__execute_line(&state, "test 1 -eq x") == 2 &&
        /* Functions: $0/$1../$# bind for the duration of the call and are
         * restored afterwards; a function shadows a builtin/external of the
         * same name. */
        shell__execute_line(&state, "greet() { shell_test_probe $1; }") == 0 &&
        shell__execute_line(&state, "greet hello") == 0 && strcmp(s_probe_arg, "hello") == 0 &&
        shell__execute_line(&state, "whoami() { shell_test_probe $0; }; whoami") == 0 &&
        strcmp(s_probe_arg, "whoami") == 0 &&
        shell__execute_line(&state, "argcount() { shell_test_probe $#; }; argcount a b c") == 0 &&
        strcmp(s_probe_arg, "3") == 0 &&
        shell__execute_line(&state, "recur() { if [ $1 -gt 0 ]; then recur 0; fi; shell_test_probe done$1; }") ==
            0 &&
        shell__execute_line(&state, "recur 1") == 0 && strcmp(s_probe_arg, "done1") == 0 &&
        /* Malformed constructs are reported, not silently misparsed. */
        shell__execute_line(&state, "if true") == 2 && shell__execute_line(&state, "if true; then echo hi") == 2;
    shell__state_free(&state);
    printf("[selftest] shell/control-flow: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_multiline_case(void) {
    if (!selftest__shell_register_probe()) return false;
    const char *path = "/apps/shell_multiline_test.sh";
    const char script[] = "greet() {\n"
                          "  # a comment inside the body\n"
                          "  if [ -n \"$1\" ]; then\n"
                          "    shell_test_probe $1\n"
                          "  else\n"
                          "    shell_test_probe empty\n"
                          "  fi\n"
                          "}\n"
                          "\n"
                          "greet multiline\n";
    (void)storage__remove(path);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    if (storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, script, sizeof(script) - 1, &written) != BRUCE_OK ||
        written != sizeof(script) - 1 || storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(path);
        return false;
    }
    char *argv[] = {"shell", (char *)path, NULL};
    int status = shell_app_main(2, argv);
    (void)storage__remove(path);
    bool ok = status == 0 && strcmp(s_probe_arg, "multiline") == 0;
    printf("[selftest] shell/multiline: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_loops_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok =
        /* (( )) arithmetic: assignment, comparison, exit status, and the
         * six comparison/logic operators actually get evaluated -- exit
         * status is 0 (true) iff the result is nonzero, matching bash. */
        shell__execute_line(&state, "x=5; (( x + 1 == 6 )) && shell_test_probe arith_ok") == 0 &&
        strcmp(s_probe_arg, "arith_ok") == 0 &&
        shell__execute_line(&state, "(( 3 * 4 - 2 ))") == 0 && shell__execute_line(&state, "(( 0 ))") == 1 &&
        shell__execute_line(&state, "(( x++ )); shell_test_probe $x") == 0 && strcmp(s_probe_arg, "6") == 0 &&
        shell__execute_line(&state, "(( x += 10 )); shell_test_probe $x") == 0 && strcmp(s_probe_arg, "16") == 0 &&
        shell__execute_line(&state, "(( 5 > 3 && 2 < 4 )) && shell_test_probe logic_ok") == 0 &&
        strcmp(s_probe_arg, "logic_ok") == 0 && shell__execute_line(&state, "(( 1 / 0 ))") == 2 &&
        /* for NAME in WORD...; do ...; done -- iterates each word, and
         * (( )) inside the body can accumulate across iterations. */
        shell__execute_line(&state, "total=0; for n in 1 2 3; do (( total += n )); done; shell_test_probe $total") ==
            0 &&
        strcmp(s_probe_arg, "6") == 0 &&
        /* break inside a for-loop's body (nested in an if) stops the loop
         * immediately, without running the rest of that iteration. */
        shell__execute_line(
            &state,
            "sum=0; for n in 1 2 3 4 5; do if [ $n -eq 3 ]; then break; fi; (( sum += n )); done; "
            "shell_test_probe $sum"
        ) == 0 &&
        strcmp(s_probe_arg, "3") == 0 &&
        /* C-style for ((init; cond; incr)). */
        shell__execute_line(
            &state, "product=1; for ((i=1; i<=4; i++)); do (( product *= i )); done; shell_test_probe $product"
        ) == 0 &&
        strcmp(s_probe_arg, "24") == 0 &&
        /* while COND; do ...; done, and a condition re-evaluated every
         * iteration off a variable the body itself mutates. */
        shell__execute_line(&state, "count=0; while [ $count -lt 3 ]; do (( count++ )); done; shell_test_probe $count") ==
            0 &&
        strcmp(s_probe_arg, "3") == 0 &&
        /* break N unwinds N enclosing loops at once. */
        shell__execute_line(
            &state,
            "count=0; for i in 1 2 3; do for j in 1 2; do (( count++ )); if [ $count -eq 1 ]; then break 2; fi; "
            "done; done; shell_test_probe $count"
        ) == 0 &&
        strcmp(s_probe_arg, "1") == 0 &&
        /* A break that's followed, in the same branch, by more structure
         * still ahead (a nested if) doesn't desync the enclosing while's
         * own then/fi/done bookkeeping -- see shell_compound__catch_up(). */
        shell__execute_line(
            &state,
            "result=start; while true; do if true; then break; if true; then result=unreached; fi; fi; done; "
            "shell_test_probe $result"
        ) == 0 &&
        strcmp(s_probe_arg, "start") == 0 &&
        /* Malformed constructs are reported, not silently misparsed. */
        shell__execute_line(&state, "for x in a b") == 2 && shell__execute_line(&state, "while true; do echo hi") == 2 &&
        shell__execute_line(&state, "(( 1 +")  == 2;
    shell__state_free(&state);
    printf("[selftest] shell/loops: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Runs `command` (a "shell -c ..." argument string) as a background child
 * with its stdio routed to a fresh session, feeds `stdin_text` into that
 * session right after launch (queued for the child's own `read` builtin --
 * see shell_builtins__read() in shell_builtins.c -- to consume whenever it
 * gets there, the same pattern selftest__run_terminal_stdio_case() uses),
 * and checks the child exits 0 having recorded `expected` via the probe. */
static bool selftest__shell_read_probe(const char *command, const char *stdin_text, const char *expected) {
    if (!selftest__shell_register_probe()) return false;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        return false;
    }
    int launched = app_runner__run("shell", command, BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0 || stdio__session_write_input(session, stdin_text, strlen(stdin_text)) != BRUCE_OK) {
        (void)stdio__session_close(session);
        return false;
    }
    bruce_process_status_t status;
    bool ok = process__wait_status((bruce_process_id_t)launched, 2000, &status) == BRUCE_OK &&
              status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0 && strcmp(s_probe_arg, expected) == 0;
    (void)stdio__session_close(session);
    return ok;
}

bool selftest__run_shell_read_case(void) {
    bool ok =
        /* A single variable gets the whole (trimmed) line. */
        selftest__shell_read_probe("-c \"read line; shell_test_probe $line\"", "hello\n", "hello") &&
        /* With several variables, the last one gets whatever's left of the
         * line, not just its next word -- matching bash's own field
         * splitting for `read`. */
        selftest__shell_read_probe(
            "-c \"read a b c; shell_test_probe $c\"", "one two three four\n", "three four"
        ) &&
        /* No variable names at all -> $REPLY. */
        selftest__shell_read_probe("-c \"read; shell_test_probe $REPLY\"", "reply-line\n", "reply-line");
    printf("[selftest] shell/read: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_stdio_inheritance_case(void) {
    if (!selftest__shell_register_probe()) return false;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        return false;
    }
    int launched = app_runner__run(
        "shell", "-c \"shell_test_probe routed\"", BRUCE_LAUNCH_BACKGROUND
    );
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    bruce_process_status_t status;
    bool completed =
        launched > 0 && process__wait_status((bruce_process_id_t)launched, 2000, &status) == BRUCE_OK;
    char output[128] = {0};
    size_t size = 0;
    bruce_result_t read = stdio__session_read_output(session, output, sizeof(output) - 1, &size);
    (void)stdio__session_close(session);
    bool ok = completed && status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0 &&
              read == BRUCE_OK && strstr(output, "shell-grandchild-routed") != NULL;
    printf("[selftest] shell/stdio-inheritance: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Confirms shell_app.c's shell__sync_tty_size actually exports $COLUMNS and
 * $LINES from the routed session's tty__get_size() -- the mechanism real
 * full-screen programs (htop, less, tmux) fall back to when they can't
 * query the terminal directly. Owns the session the same way terminal_app.c
 * does: tty__set_size() before routing children, so the spawned "shell -i"
 * (and its own "shell_test_probe" grandchild) see the size from their very
 * first read. */
static bool
selftest__shell_tty_size_probe(bruce_stdio_session_t session, const char *command, const char *expected) {
    memset(s_probe_arg, 0, sizeof(s_probe_arg));
    if (stdio__session_route_children(session) != BRUCE_OK) return false;
    int launched = app_runner__run("shell", command, BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    bruce_process_status_t status;
    return launched > 0 &&
           process__wait_status((bruce_process_id_t)launched, 2000, &status) == BRUCE_OK &&
           status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0 && strcmp(s_probe_arg, expected) == 0;
}

bool selftest__run_shell_tty_size_case(void) {
    if (!selftest__shell_register_probe()) return false;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK) return false;
    bool ok = tty__set_size(session, 100, 40) == BRUCE_OK &&
              selftest__shell_tty_size_probe(session, "-c \"shell_test_probe $COLUMNS\"", "100") &&
              selftest__shell_tty_size_probe(session, "-c \"shell_test_probe $LINES\"", "40");
    (void)stdio__session_close(session);
    printf("[selftest] shell/tty-size: %s\n", ok ? "OK" : "failed");
    return ok;
}
