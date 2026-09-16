#include "shell_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"
#include "modules/shell/shell_app.h"
#include "modules/shell/shell_builtins.h"
#include "modules/shell/shell_console.h"
#include "modules/shell/shell_history.h"
#include "modules/shell/shell_internal.h"
#include "modules/utils/terminal/terminal_ansi.h"

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

/* Runs until signaled (INT/TERM) or killed -- unlike selftest__shell_probe(),
 * which returns immediately and so can't be caught still "Running" by a
 * "kill" test. runtime__delay() stops returning BRUCE_OK once a signal is
 * pending, same mechanism selftest__worker_clears_signal() in process_test.c
 * relies on. */
static int selftest__shell_probe_spin(int argc, char **argv) {
    (void)argc;
    (void)argv;
    while (runtime__delay(5) == BRUCE_OK) {}
    return 0;
}

static bool selftest__shell_register_spin_probe(void) {
    bruce_result_t result = app_runner__register(
        "shell_test_spin", "Shell integration spin probe", "Test", selftest__shell_probe_spin, 0
    );
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
        /* $$ -- this shell's own pid, always set. */
        shell__execute_line(&state, "shell_test_probe $$") == 0 &&
        atoi(s_probe_arg) == (int)process__current_id() &&
        /* $! -- unset (0) until the first "cmd &"/"func &"; the job-control
         * selftest case covers it actually tracking a real background pid. */
        shell__execute_line(&state, "shell_test_probe $!") == 0 && strcmp(s_probe_arg, "0") == 0 &&
        shell__execute_line(&state, "echo broken | echo nope") == 2 &&
        /* "echo > file" now succeeds -- builtin/function output redirection
         * is supported (shell_executor__builtin_redirected()), so this just
         * writes an empty line to the target rather than being rejected. */
        shell__execute_line(&state, "echo > /apps/shell_language_echo_redirect.txt") == 0 &&
        shell__execute_line(&state, "echo 'unterminated") == 2;
    if (ok) ok = shell__execute_line(&state, "shell_test_probe $INHERITED_SHELL visible") == 0;
    shell__state_free(&state);
    (void)storage__remove("/apps/shell_language_echo_redirect.txt");
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

/* Sets up a real file and a real directory under /apps and exercises
 * test/['s -e/-f/-d/-r/-w/-x unary file-test operators against them
 * (shell_condition.c's shell_condition__file_test()) -- called from
 * selftest__run_shell_control_flow_case() below, which already owns the
 * shell_state_t these lines run against. */
static bool selftest__shell_condition_file_tests(shell_state_t *state) {
    const char *file_path = "/apps/shell_condition_test.txt";
    const char *dir_path = "/apps/shell_condition_test_dir";
    const char *missing_path = "/apps/shell_condition_test_missing.txt";
    (void)storage__remove(file_path);
    (void)storage__remove(dir_path);
    (void)storage__remove(missing_path);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    static const char contents[] = "probe";
    if (storage__open(
            file_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, contents, sizeof(contents) - 1, &written) != BRUCE_OK ||
        written != sizeof(contents) - 1 || storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(file_path);
        return false;
    }
    if (storage__mkdir(dir_path) != BRUCE_OK) {
        (void)storage__remove(file_path);
        return false;
    }

    bool ok =
        shell__execute_line(state, "[ -e /apps/shell_condition_test.txt ] && shell_test_probe exists") == 0 &&
        strcmp(s_probe_arg, "exists") == 0 &&
        shell__execute_line(state, "[ -e /apps/shell_condition_test_missing.txt ] && shell_test_probe skipped") ==
            1 &&
        shell__execute_line(state, "[ -f /apps/shell_condition_test.txt ] && shell_test_probe is_file") == 0 &&
        strcmp(s_probe_arg, "is_file") == 0 &&
        shell__execute_line(state, "[ -f /apps/shell_condition_test_dir ] && shell_test_probe skipped") == 1 &&
        shell__execute_line(state, "[ -d /apps/shell_condition_test_dir ] && shell_test_probe is_dir") == 0 &&
        strcmp(s_probe_arg, "is_dir") == 0 &&
        shell__execute_line(state, "[ -d /apps/shell_condition_test.txt ] && shell_test_probe skipped") == 1 &&
        shell__execute_line(state, "[ -r /apps/shell_condition_test.txt ] && shell_test_probe readable") == 0 &&
        strcmp(s_probe_arg, "readable") == 0 &&
        shell__execute_line(state, "[ -w /apps/shell_condition_test.txt ] && shell_test_probe writable") == 0 &&
        strcmp(s_probe_arg, "writable") == 0 &&
        shell__execute_line(state, "[ -x /apps/shell_condition_test_dir ] && shell_test_probe traversable") == 0 &&
        strcmp(s_probe_arg, "traversable") == 0 &&
        /* Relative paths resolve against $PWD, same as any other shell path
         * argument. */
        shell__execute_line(state, "cd /apps; [ -f shell_condition_test.txt ] && shell_test_probe relative") ==
            0 &&
        strcmp(s_probe_arg, "relative") == 0 &&
        shell__execute_line(state, "cd /") == 0;
    /* -w's non-destructive open-for-write probe never truncates or creates
     * content -- the file's own contents must still be exactly what was
     * written above. */
    if (ok) {
        char readback[16] = {0};
        size_t read_size = 0;
        bruce_file_id_t read_file = BRUCE_FILE_ID_INVALID;
        ok = storage__open(file_path, BRUCE_STORAGE_OPEN_READ, &read_file) == BRUCE_OK &&
             storage__read(read_file, readback, sizeof(readback) - 1, &read_size) == BRUCE_OK &&
             storage__close(read_file) == BRUCE_OK && read_size == sizeof(contents) - 1 &&
             memcmp(readback, contents, read_size) == 0;
    }
    (void)storage__remove(file_path);
    (void)storage__remove(dir_path);
    if (!ok) printf("[selftest] shell/control-flow: file-test operators failed\n");
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
        /* A taken branch's exit status is that of its own glued "then X"/
         * "else X" command when nothing else follows it before "fi" --
         * regression test for a bug where shell_compound__run_if() clobbered
         * this status with 0 (the empty, never-executed remainder after it)
         * whenever the glued command's own status was nonzero. Masked for
         * years because shell_test_probe (used almost everywhere else in
         * this file) always returns 0, so the clobber was invisible until a
         * real nonzero-status command exposed it. */
        shell__execute_line(&state, "if true; then false; fi") == 1 &&
        shell__execute_line(&state, "if false; then true; else false; fi") == 1 &&
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
        /* -o (mirrors -a) for test/[. */
        shell__execute_line(&state, "[ 1 -eq 2 -o 3 -eq 3 ] && shell_test_probe or_true") == 0 &&
        strcmp(s_probe_arg, "or_true") == 0 &&
        shell__execute_line(&state, "[ 1 -eq 2 -o 3 -eq 4 ] && shell_test_probe skipped") == 1 &&
        /* "[[ COND1 && COND2 ]]" / "[[ COND1 || COND2 ]]": shell_parser__plan()
         * normally tokenizes a top-level "&&"/"||" as a command connector,
         * splitting the line into short-circuited commands -- its own
         * "[[ ... ]]" span tracking keeps this one flat command instead, so
         * it reaches shell_condition__run() intact. */
        shell__execute_line(&state, "[[ 1 -eq 1 && 2 -eq 2 ]] && shell_test_probe dbracket_and") == 0 &&
        strcmp(s_probe_arg, "dbracket_and") == 0 &&
        shell__execute_line(&state, "[[ 1 -eq 2 || 2 -eq 2 ]] && shell_test_probe dbracket_or") == 0 &&
        strcmp(s_probe_arg, "dbracket_or") == 0 &&
        shell__execute_line(&state, "[[ 1 -eq 2 && 2 -eq 2 ]] && shell_test_probe skipped") == 1 &&
        /* "&&" binds tighter than "||": "A && B || C" reads as "(A && B) ||
         * C", so this is true even though the "&&" side alone is false. */
        shell__execute_line(&state, "[[ 1 -eq 2 && 2 -eq 2 || 3 -eq 3 ]] && shell_test_probe precedence") == 0 &&
        strcmp(s_probe_arg, "precedence") == 0 &&
        /* File-test unary operators (-e/-f/-d/-r/-w/-x), resolved against
         * $PWD like any other shell path argument. */
        selftest__shell_condition_file_tests(&state) &&
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

/* Exercises `local`'s function-call scoping (shell_builtins.c's
 * shell_builtins__local()/shell_local_frame_t, restored by
 * shell_compound__call_function() in shell_compound.c): a variable localized
 * inside a function shadows the outer one only for that call's own extent
 * (and any call it makes in turn -- like bash, this is dynamic scoping on
 * the same flat variable table, not lexical), and reverts -- or is fully
 * removed, if it never existed outside -- the moment the call returns. */
bool selftest__run_shell_local_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok =
        /* A local shadows an existing outer variable while the call runs,
         * and the outer value comes back once it returns. */
        shell__execute_line(&state, "x=outer") == 0 &&
        shell__execute_line(&state, "f() { local x=inner; shell_test_probe $x; }") == 0 &&
        shell__execute_line(&state, "f") == 0 && strcmp(s_probe_arg, "inner") == 0 &&
        shell__execute_line(&state, "shell_test_probe $x") == 0 && strcmp(s_probe_arg, "outer") == 0 &&
        /* A function `h` calls sees `h`'s local (dynamic scoping on the same
         * flat variable table) and can reassign it in place with no `local`
         * of its own -- that reassignment is visible to `h` once the callee
         * returns, same as any other non-local assignment would be -- but it
         * still reverts once `h` itself returns. */
        shell__execute_line(&state, "g() { x=from-g; }; h() { local x=h-local; g; shell_test_probe $x; }") ==
            0 &&
        shell__execute_line(&state, "h") == 0 && strcmp(s_probe_arg, "from-g") == 0 &&
        shell__execute_line(&state, "shell_test_probe $x") == 0 && strcmp(s_probe_arg, "outer") == 0 &&
        /* A bare "local NAME" (no "=value") starts out empty, distinct from
         * whatever the outer variable holds. */
        shell__execute_line(&state, "j() { local x; shell_test_probe \"$x\"; }") == 0 &&
        shell__execute_line(&state, "j") == 0 && strcmp(s_probe_arg, "") == 0 &&
        shell__execute_line(&state, "shell_test_probe $x") == 0 && strcmp(s_probe_arg, "outer") == 0 &&
        /* A name localized that never existed outside the call is fully
         * removed, not left behind holding "", once the call returns. */
        shell__execute_line(&state, "k() { local brand_new=temp; shell_test_probe $brand_new; }") == 0 &&
        shell__execute_line(&state, "k") == 0 && strcmp(s_probe_arg, "temp") == 0 &&
        shell_builtins__get(&state, "brand_new") == NULL &&
        /* Recursive calls each get their own frame: a name localized deeper
         * in the recursion reverts on the way back out of *that* call only,
         * restoring the enclosing call's own local value every time a
         * deeper call returns rather than the outermost caller's. */
        shell__execute_line(
            &state, "r() { local x=$1; n=$1; if [ $n -gt 1 ]; then ((n = n - 1)); r $n; fi; shell_test_probe $x; }"
        ) == 0 &&
        shell__execute_line(&state, "r 3") == 0 && strcmp(s_probe_arg, "3") == 0 &&
        /* Using `local` outside of any function call is rejected rather than
         * silently acting like a plain assignment. */
        shell__execute_line(&state, "local y=nope") == 1 && shell_builtins__get(&state, "y") == NULL;
    shell__state_free(&state);
    printf("[selftest] shell/local: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises "$(...)" / "`...`" command substitution (shell_parser.c's
 * shell_parser__substitution_span()/shell_parser__splice_substitution(),
 * driven by shell_executor__run_substitution() in shell_executor.c): its
 * content is run as a nested "shell -c" child process, its trailing-newline-
 * stripped stdout is spliced in as one unsplit word, a nonzero exit discards
 * whatever it printed, and -- being a real separate process rather than a
 * copy-on-write subshell -- it sees exported variables and the filesystem
 * but never the calling shell's own unexported variables or functions. */
static bool selftest__shell_substitution_step(
    bool ok_so_far, shell_state_t *state, const char *line, int expected_status, const char *expected_arg
) {
    if (!ok_so_far) return false;
    int status = shell__execute_line(state, line);
    bool ok = status == expected_status && (expected_arg == NULL || strcmp(s_probe_arg, expected_arg) == 0);
    if (!ok) {
        printf(
            "[selftest] shell/command-substitution: `%s` -> status=%d (want %d) arg=\"%s\" (want \"%s\")\n",
            line,
            status,
            expected_status,
            s_probe_arg,
            expected_arg != NULL ? expected_arg : "(unchecked)"
        );
    }
    return ok;
}

/* Like selftest__shell_substitution_step() above, but for QEMU mode's
 * relaxed content check: only requires the substitution produced *some*
 * non-empty word (see selftest__run_shell_command_substitution_case()'s own
 * CONFIG_BRUCE_QEMU_TEST_MODE comment for why exact bytes aren't checked
 * there). */
static bool selftest__shell_substitution_nonempty_step(bool ok_so_far, shell_state_t *state, const char *line) {
    if (!ok_so_far) return false;
    int status = shell__execute_line(state, line);
    bool ok = status == 0 && strlen(s_probe_arg) > 0;
    if (!ok) {
        printf(
            "[selftest] shell/command-substitution: `%s` -> status=%d (want 0) arg=\"%s\" (want non-empty)\n", line,
            status, s_probe_arg
        );
    }
    return ok;
}

/* Like the two step helpers above, but for QEMU mode's relaxed isolation
 * check: a successful substitution's captured bytes aren't verified exactly
 * (see selftest__run_shell_command_substitution_case()'s
 * CONFIG_BRUCE_QEMU_TEST_MODE comment), so a leaked value can't be checked
 * for exact absence ("") either -- only that whatever came back isn't the
 * verbatim `forbidden_value` a real leak would have produced, which swap
 * corruption reproducing by coincidence is astronomically unlikely. */
static bool selftest__shell_substitution_not_leaked_step(
    bool ok_so_far, shell_state_t *state, const char *line, const char *forbidden_value
) {
    if (!ok_so_far) return false;
    int status = shell__execute_line(state, line);
    bool ok = status == 0 && strcmp(s_probe_arg, forbidden_value) != 0;
    if (!ok) {
        printf(
            "[selftest] shell/command-substitution: `%s` -> status=%d (want 0) arg=\"%s\" (leaked \"%s\")\n", line,
            status, s_probe_arg, forbidden_value
        );
    }
    return ok;
}

bool selftest__run_shell_command_substitution_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok = true;
#if CONFIG_BRUCE_QEMU_TEST_MODE
    /* shell_executor__run_substitution() captures its child's output through
     * the same memory__external_malloc()-backed buffer every other captured-
     * output path in this shell uses -- and that backing is unreliable under
     * QEMU's swap-backend fallback (see selftest__run_shell_bnu_text_pipe_case()
     * above, and shell_executor__run_substitution()'s own doc comment): a
     * *successful* substitution's captured bytes can come back corrupted
     * even though spawning, routing, draining, and splicing all worked. So
     * under QEMU these only check that a successful substitution produced
     * *some* non-empty word, not its exact bytes. */
    ok = selftest__shell_substitution_nonempty_step(ok, &state, "shell_test_probe $(echo hi)");
    ok = selftest__shell_substitution_nonempty_step(ok, &state, "shell_test_probe `echo hi`");
    ok = selftest__shell_substitution_nonempty_step(ok, &state, "shell_test_probe $(echo $(echo nested))");
    ok = selftest__shell_substitution_nonempty_step(ok, &state, "shell_test_probe \"$(echo a b)\"");
    ok = selftest__shell_substitution_nonempty_step(ok, &state, "export ev=visible; shell_test_probe $(echo $ev)");
    /* ... and the calling shell's own unexported variables are not visible
     * to that child process -- under QEMU, only checked as "didn't leak the
     * verbatim value" (see the helper's own comment), not exact emptiness,
     * since this goes through the same unreliable captured-content path as
     * the checks above (echo succeeds either way, so this never reaches the
     * exact/reliable discard-on-nonzero-exit path below). */
    ok = selftest__shell_substitution_not_leaked_step(
        ok, &state, "secret=hidden; y=$(echo $secret); shell_test_probe \"$y\"", "hidden"
    );
#else
    /* Basic "$(...)" and the "`...`" spelling. */
    ok = selftest__shell_substitution_step(ok, &state, "shell_test_probe $(echo hi) hi", 0, "hi");
    ok = selftest__shell_substitution_step(ok, &state, "shell_test_probe `echo hi` hi", 0, "hi");
    /* Substitutions nest. */
    ok = selftest__shell_substitution_step(ok, &state, "shell_test_probe $(echo $(echo nested)) nested", 0, "nested");
    /* Recognized inside double quotes too, and -- like a plain $VAR
     * expansion -- never itself word-split: embedded whitespace in the
     * captured output survives as part of one word. */
    ok = selftest__shell_substitution_step(ok, &state, "shell_test_probe \"$(echo a b)\" \"a b\"", 0, "a b");
    /* It runs in a real, separate child process: exported variables are
     * visible ... */
    ok = selftest__shell_substitution_step(
        ok, &state, "export ev=visible; shell_test_probe $(echo $ev) visible", 0, "visible"
    );
    /* ... but the calling shell's own unexported variables are not visible
     * to that child process ... */
    ok = selftest__shell_substitution_step(
        ok, &state, "secret=hidden; y=$(echo $secret); shell_test_probe \"$y\" \"\"", 0, ""
    );
#endif
    /* A substitution whose command exits nonzero discards whatever it
     * printed, matching shell_executor__capture_external()'s existing
     * discard-on-failure rule for redirection/pipes -- exact and reliable
     * regardless of backend, since that path never touches the captured-
     * output buffer's content at all (it's left at {0}, see
     * shell_executor__capture_external()'s early return on a nonzero exit). */
    ok = selftest__shell_substitution_step(ok, &state, "x=$(echo hi; false); shell_test_probe \"$x\" \"\"", 0, "");
    /* ... and neither are its function definitions -- calling one is "not
     * found", exactly like invoking it from any other external command's
     * child process, so its output is discarded the same as any other
     * nonzero-exit substitution. */
    ok = selftest__shell_substitution_step(
        ok, &state, "myfunc() { echo from-func; }; w=$(myfunc); shell_test_probe \"$w\" \"\"", 0, ""
    );

    shell__state_free(&state);
    printf("[selftest] shell/command-substitution: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises "$((...))" arithmetic expansion as a *word* (shell_parser.c's
 * doubled-paren detection in shell_parser__expand()'s "$(" branch, driven by
 * shell_executor__eval_arith_word() in shell_executor.c) -- unlike
 * "$(...)"/"`...`" command substitution above, this never spawns a nested
 * process, so it's not subject to that feature's QEMU swap-backend caveat,
 * and an assignment inside it (e.g. "$((x = 5))") mutates this shell's own
 * `x` exactly like the standalone "((...))" statement form does. */
bool selftest__run_shell_arith_word_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok =
        /* Basic expansion, and nested parens/precedence. */
        shell__execute_line(&state, "shell_test_probe $((1 + 2)) 3") == 0 &&
        shell__execute_line(&state, "shell_test_probe $((2 * (3 + 4))) 14") == 0 &&
        /* Recognized inside double quotes too, same as "$(...)". */
        shell__execute_line(&state, "shell_test_probe \"$((1 + 1))\" 2") == 0 &&
        /* Splices into a word alongside surrounding literal text, same as
         * any other "$..." expansion. */
        shell__execute_line(&state, "shell_test_probe a$((1 + 1))b a2b") == 0 &&
        /* An assignment inside it is a real side effect on this shell's own
         * variable, not something scoped to the expansion -- distinguishing
         * it from "$(...)"'s nested-process isolation. */
        shell__execute_line(&state, "shell_test_probe $((y = 10)) 10") == 0 &&
        shell__execute_line(&state, "shell_test_probe $y 10") == 0 &&
        /* A real arithmetic error (not just a malformed expansion) is
         * reported and rejects the whole command, matching the standalone
         * "((...))" statement form's own "shell: ((: ...\n" / status 2
         * behavior for the same input. */
        shell__execute_line(&state, "shell_test_probe $((1 / 0))") == 2;

    shell__state_free(&state);
    printf("[selftest] shell/arith-word: %s\n", ok ? "OK" : "failed");
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
                          "greet multiline\n"
                          /* A real multi-line loop, not just if/fi and a
                           * function body -- exercises shell_compound__pending()'s
                           * own loop_depth counter (incremented on "for"/
                           * "while"/"until", decremented on "done"), which
                           * decides whether shell_app.c keeps reading more
                           * lines before running this at all. */
                          "count=0\n"
                          "until [ $count -ge 2 ]; do\n"
                          "  count=$((count + 1))\n"
                          "done\n"
                          "shell_test_probe count-$count\n";
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
    bool ok = status == 0 && strcmp(s_probe_arg, "count-2") == 0;
    printf("[selftest] shell/multiline: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Runs one line and checks its exit status (and, if expected_arg is not
 * NULL, that shell_test_probe's last argument matches). Prints exactly
 * which checkpoint failed and what actually happened, then reports whether
 * this and every prior checkpoint in the case passed. */
static bool selftest__shell_loops_step(
    bool ok_so_far, shell_state_t *state, const char *line, int expected_status, const char *expected_arg
) {
    if (!ok_so_far) return false;
    int status = shell__execute_line(state, line);
    bool ok = status == expected_status && (expected_arg == NULL || strcmp(s_probe_arg, expected_arg) == 0);
    if (!ok) {
        printf(
            "[selftest] shell/loops: `%s` -> status=%d (want %d) arg=\"%s\" (want \"%s\")\n",
            line,
            status,
            expected_status,
            s_probe_arg,
            expected_arg != NULL ? expected_arg : "(unchecked)"
        );
    }
    return ok;
}

bool selftest__run_shell_loops_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok = true;
    /* (( )) arithmetic: assignment, comparison, exit status, and the six
     * comparison/logic operators actually get evaluated -- exit status is 0
     * (true) iff the result is nonzero, matching bash. */
    ok = selftest__shell_loops_step(
        ok, &state, "x=5; (( x + 1 == 6 )) && shell_test_probe arith_ok", 0, "arith_ok"
    );
    ok = selftest__shell_loops_step(ok, &state, "(( 3 * 4 - 2 ))", 0, NULL);
    ok = selftest__shell_loops_step(ok, &state, "(( 0 ))", 1, NULL);
    ok = selftest__shell_loops_step(ok, &state, "(( x++ )); shell_test_probe $x", 0, "6");
    ok = selftest__shell_loops_step(ok, &state, "(( x += 10 )); shell_test_probe $x", 0, "16");
    ok = selftest__shell_loops_step(
        ok, &state, "(( 5 > 3 && 2 < 4 )) && shell_test_probe logic_ok", 0, "logic_ok"
    );
    ok = selftest__shell_loops_step(ok, &state, "(( 1 / 0 ))", 2, NULL);
    /* for NAME in WORD...; do ...; done -- iterates each word, and (( ))
     * inside the body can accumulate across iterations. */
    ok = selftest__shell_loops_step(
        ok, &state, "total=0; for n in 1 2 3; do (( total += n )); done; shell_test_probe $total", 0, "6"
    );
    /* break inside a for-loop's body (nested in an if) stops the loop
     * immediately, without running the rest of that iteration. */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "sum=0; for n in 1 2 3 4 5; do if [ $n -eq 3 ]; then break; fi; (( sum += n )); done; "
        "shell_test_probe $sum",
        0,
        "3"
    );
    /* C-style for ((init; cond; incr)). */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "product=1; for ((i=1; i<=4; i++)); do (( product *= i )); done; shell_test_probe $product",
        0,
        "24"
    );
    /* while COND; do ...; done, and a condition re-evaluated every iteration
     * off a variable the body itself mutates. */
    ok = selftest__shell_loops_step(
        ok, &state, "count=0; while [ $count -lt 3 ]; do (( count++ )); done; shell_test_probe $count", 0, "3"
    );
    /* until COND; do ...; done -- the same loop as while, just stopping once
     * COND *succeeds* instead of once it fails. */
    ok = selftest__shell_loops_step(
        ok, &state, "count=0; until [ $count -ge 3 ]; do (( count++ )); done; shell_test_probe $count", 0, "3"
    );
    /* A COND that's already true the first time never runs the body at all,
     * same as "while false; do ...; done" never does. */
    ok = selftest__shell_loops_step(
        ok, &state, "result=untouched; until true; do result=ran; done; shell_test_probe $result", 0, "untouched"
    );
    /* break N unwinds N enclosing loops at once. */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "count=0; for i in 1 2 3; do for j in 1 2; do (( count++ )); if [ $count -eq 1 ]; then break 2; fi; "
        "done; done; shell_test_probe $count",
        0,
        "1"
    );
    /* A break that's followed, in the same branch, by more structure still
     * ahead (a nested if) doesn't desync the enclosing while's own
     * then/fi/done bookkeeping -- see shell_compound__catch_up(). */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "result=start; while true; do if true; then break; if true; then result=unreached; fi; fi; done; "
        "shell_test_probe $result",
        0,
        "start"
    );
    /* continue inside a for-loop's body skips the rest of *that* iteration
     * only -- the loop itself keeps going, unlike break. */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "sum=0; for n in 1 2 3 4 5; do if [ $n -eq 3 ]; then continue; fi; (( sum += n )); done; "
        "shell_test_probe $sum",
        0,
        "12"
    );
    /* continue in a C-style for-loop still runs the loop's own increment
     * clause before re-testing its condition -- if it didn't, skipping i=2
     * this way would spin forever instead of finishing at i=5. */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "count=0; for ((i=0; i<5; i++)); do if [ $i -eq 2 ]; then continue; fi; (( count++ )); done; "
        "shell_test_probe $count",
        0,
        "4"
    );
    /* continue N unwinds N-1 enclosing loops on its way out, then continues
     * the Nth (target) loop's next iteration rather than stopping it --
     * here, "continue 2" from inside the inner loop skips straight to the
     * outer loop's next `i`, without the inner loop's j=3 ever running. */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "result=; for i in 1 2 3; do for j in 1 2 3; do if [ $j -eq 2 ]; then continue 2; fi; "
        "result=\"$result$i-$j \"; done; done; shell_test_probe \"$result\"",
        0,
        "1-1 2-1 3-1 "
    );
    /* A stray continue outside any loop is reported, not silently treated as
     * an "unexpected token" (or worse, left to desync the next run()). */
    ok = selftest__shell_loops_step(ok, &state, "continue", 0, NULL);
    /* return, bare, uses $?'s value going in (here, `true`'s 0) and stops
     * the function immediately -- the "unreached" probe after it never
     * runs, leaving the sentinel arg set just before the call untouched. */
    ok = selftest__shell_loops_step(ok, &state, "shell_test_probe sentinel", 0, "sentinel");
    ok = selftest__shell_loops_step(
        ok, &state, "retbare() { true; return; shell_test_probe unreached; }; retbare", 0, "sentinel"
    );
    /* return N reports that exact status instead. */
    ok = selftest__shell_loops_step(ok, &state, "retval() { return 7; }; retval", 7, NULL);
    /* A return nested inside a loop/if unwinds straight out of the whole
     * function -- not just that loop -- so neither the loop's later
     * iterations (n=3's probe) nor anything after the loop ("unreached")
     * ever run: the last probe call is still n=1's. */
    ok = selftest__shell_loops_step(
        ok,
        &state,
        "retloop() { for n in 1 2 3; do if [ $n -eq 2 ]; then return 5; fi; shell_test_probe $n; done; "
        "shell_test_probe unreached; }; retloop",
        5,
        "1"
    );
    /* ...but the *caller's* own next statement still runs: the unwind stops
     * exactly at the call boundary, not one level further out. */
    ok = selftest__shell_loops_step(ok, &state, "retloop; shell_test_probe after", 0, "after");
    /* A bare return outside any function call is rejected outright, matching
     * bash's "return: can only `return' from a function". */
    ok = selftest__shell_loops_step(ok, &state, "return", 1, NULL);
    /* shift, bare (N=1 default), drops $1 and shifts $2.. down, updating $#
     * to match. */
    ok = selftest__shell_loops_step(
        ok, &state, "shiftone() { shift; shell_test_probe \"$1:$#\"; }; shiftone a b c", 0, "b:2"
    );
    /* shift N drops the first N. */
    ok = selftest__shell_loops_step(
        ok, &state, "shifttwo() { shift 2; shell_test_probe \"$1:$#\"; }; shifttwo a b c d", 0, "c:2"
    );
    /* shift 0 is a no-op. */
    ok = selftest__shell_loops_step(
        ok, &state, "shiftzero() { shift 0; shell_test_probe \"$1:$#\"; }; shiftzero a b", 0, "a:2"
    );
    /* Shifting past $# is an error (bash's "shift count out of range") ... */
    ok = selftest__shell_loops_step(ok, &state, "shiftbig() { shift 5; }; shiftbig a b", 1, NULL);
    /* ...that leaves the positional parameters untouched rather than
     * partially shifting or corrupting them. */
    ok = selftest__shell_loops_step(
        ok, &state, "shiftbig2() { shift 5; shell_test_probe \"$#\"; }; shiftbig2 a b", 0, "2"
    );
    /* Malformed constructs are reported, not silently misparsed. */
    ok = selftest__shell_loops_step(ok, &state, "for x in a b", 2, NULL);
    ok = selftest__shell_loops_step(ok, &state, "while true; do echo hi", 2, NULL);
    ok = selftest__shell_loops_step(ok, &state, "until false; do echo hi", 2, NULL);
    ok = selftest__shell_loops_step(ok, &state, "(( 1 +", 2, NULL);

    shell__state_free(&state);
    printf("[selftest] shell/loops: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Runs one line and checks its exit status (and, if expected_arg is not
 * NULL, that shell_test_probe's last argument matches) -- same shape as
 * selftest__shell_loops_step() above, just labeled for this case. */
static bool selftest__shell_case_step(
    bool ok_so_far, shell_state_t *state, const char *line, int expected_status, const char *expected_arg
) {
    if (!ok_so_far) return false;
    int status = shell__execute_line(state, line);
    bool ok = status == expected_status && (expected_arg == NULL || strcmp(s_probe_arg, expected_arg) == 0);
    if (!ok) {
        printf(
            "[selftest] shell/case: `%s` -> status=%d (want %d) arg=\"%s\" (want \"%s\")\n",
            line,
            status,
            expected_status,
            s_probe_arg,
            expected_arg != NULL ? expected_arg : "(unchecked)"
        );
    }
    return ok;
}

/* Exercises `case WORD in PATTERN[|PATTERN...]) commands ;; ... esac`
 * (shell_compound.c's shell_compound__run_case(), matching each clause with
 * shell_glob__match() in shell_glob.c): single/multiple clauses,
 * "|"-separated alternative patterns, "*"/"?"/"[...]" glob wildcards, a
 * "*)" catch-all, no match with no catch-all (status 0, same as bash), only
 * the first matching clause's body runs (lazily -- a later clause's own
 * "$(...)" pattern never executes), nested case-in-case and case-in-loop,
 * and malformed-construct errors. */
bool selftest__run_shell_case_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok = true;
    /* One-line clauses, and only the matching clause's body runs. */
    ok = selftest__shell_case_step(
        ok, &state, "case b in a) shell_test_probe skipped ;; b) shell_test_probe matched ;; esac", 0, "matched"
    );
    /* "|"-separated alternative patterns. */
    ok = selftest__shell_case_step(
        ok, &state, "case y in x|y|z) shell_test_probe alt ;; esac", 0, "alt"
    );
    /* Glob wildcards: "*", "?", and a bracket class. */
    ok = selftest__shell_case_step(ok, &state, "case hello in h*o) shell_test_probe star ;; esac", 0, "star");
    ok = selftest__shell_case_step(ok, &state, "case cat in c?t) shell_test_probe question ;; esac", 0, "question");
    ok = selftest__shell_case_step(
        ok, &state, "case b in [abc]) shell_test_probe bracket ;; esac", 0, "bracket"
    );
    ok = selftest__shell_case_step(
        ok, &state, "case d in [abc]) shell_test_probe skipped ;; *) shell_test_probe catchall ;; esac", 0,
        "catchall"
    );
    /* No match and no catch-all: status 0, same as bash, and the probe
     * doesn't fire. */
    s_probe_calls = 0;
    ok = selftest__shell_case_step(ok, &state, "case z in a) shell_test_probe skipped ;; esac", 0, NULL) &&
         s_probe_calls == 0;
    /* Case word/patterns are expanded like inside double quotes (variables
     * substitute, but the result isn't itself word-split or globbed against
     * the filesystem). */
    ok = selftest__shell_case_step(
        ok, &state, "w=foo; case $w in foo) shell_test_probe var_word ;; esac", 0, "var_word"
    );
    /* Multi-line form, with a multi-command clause body spanning ";". */
    ok = selftest__shell_case_step(
        ok,
        &state,
        "case 2 in\n"
        "  1) shell_test_probe one ;;\n"
        "  2) x=mid; shell_test_probe $x ;;\n"
        "  *) shell_test_probe skipped ;;\n"
        "esac",
        0,
        "mid"
    );
    /* Nested case-in-case, and case-in-loop composition. */
    ok = selftest__shell_case_step(
        ok,
        &state,
        "case a in a) case b in b) shell_test_probe nested ;; esac ;; esac",
        0,
        "nested"
    );
    ok = selftest__shell_case_step(
        ok,
        &state,
        "sum=0; for n in 1 2 3; do case $n in 2) (( sum += 10 )) ;; *) (( sum += n )) ;; esac; done; "
        "shell_test_probe $sum",
        0,
        "14"
    );
    /* A later, never-reached clause's pattern never runs -- its "$(...)"
     * side effect (setting `w`) never fires since the first clause already
     * matched. */
    ok = selftest__shell_case_step(
        ok,
        &state,
        "w=unset; case a in a) shell_test_probe first ;; $(w=ran; echo a)) shell_test_probe skipped ;; esac; "
        "shell_test_probe $w",
        0,
        "unset"
    );
    /* Malformed constructs are reported, not silently misparsed. */
    ok = selftest__shell_case_step(ok, &state, "case a in a) echo hi ;;", 2, NULL);
    ok = selftest__shell_case_step(ok, &state, "case a of a) echo hi ;; esac", 2, NULL);
    ok = selftest__shell_case_step(ok, &state, "case a in a echo hi ;; esac", 2, NULL);

    shell__state_free(&state);
    printf("[selftest] shell/case: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Sets up a real scratch directory under /apps and exercises real pathname
 * (glob) expansion (shell_parser.c's shell_parser__words(), backed by
 * shell_glob.c) -- called from selftest__run_shell_glob_case() below, which
 * owns the shell_state_t these lines run against. */
static bool selftest__shell_glob_fixture_teardown(void) {
    bool ok = true;
    ok = storage__remove("/apps/shell_glob_test_dir/sub/inner.txt") == BRUCE_OK && ok;
    ok = storage__remove("/apps/shell_glob_test_dir/sub") == BRUCE_OK && ok;
    ok = storage__remove("/apps/shell_glob_test_dir/a.txt") == BRUCE_OK && ok;
    ok = storage__remove("/apps/shell_glob_test_dir/b.txt") == BRUCE_OK && ok;
    ok = storage__remove("/apps/shell_glob_test_dir/z.txt") == BRUCE_OK && ok;
    ok = storage__remove("/apps/shell_glob_test_dir/.hidden") == BRUCE_OK && ok;
    ok = storage__remove("/apps/shell_glob_test_dir") == BRUCE_OK && ok;
    return ok;
}

static bool selftest__shell_glob_write(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    bool ok = storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file) ==
                  BRUCE_OK &&
              storage__write(file, "x", 1, &written) == BRUCE_OK && written == 1 && storage__close(file) == BRUCE_OK;
    if (!ok && file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
    return ok;
}

static bool selftest__shell_glob_fixture_setup(void) {
    /* Best-effort clean slate: a previous run that crashed mid-test could
     * have left any of these behind. */
    (void)selftest__shell_glob_fixture_teardown();
    return storage__mkdir("/apps/shell_glob_test_dir") == BRUCE_OK &&
           storage__mkdir("/apps/shell_glob_test_dir/sub") == BRUCE_OK &&
           selftest__shell_glob_write("/apps/shell_glob_test_dir/a.txt") &&
           selftest__shell_glob_write("/apps/shell_glob_test_dir/b.txt") &&
           selftest__shell_glob_write("/apps/shell_glob_test_dir/z.txt") &&
           selftest__shell_glob_write("/apps/shell_glob_test_dir/.hidden") &&
           selftest__shell_glob_write("/apps/shell_glob_test_dir/sub/inner.txt");
}

/* Like selftest__shell_case_step()/selftest__shell_loops_step() above, for
 * selftest__run_shell_glob_case() below. */
static bool selftest__shell_glob_step(
    bool ok_so_far, shell_state_t *state, const char *line, int expected_status, const char *expected_arg
) {
    if (!ok_so_far) return false;
    int status = shell__execute_line(state, line);
    bool ok = status == expected_status && (expected_arg == NULL || strcmp(s_probe_arg, expected_arg) == 0);
    if (!ok) {
        printf(
            "[selftest] shell/glob: `%s` -> status=%d (want %d) arg=\"%s\" (want \"%s\")\n", line, status,
            expected_status, s_probe_arg, expected_arg != NULL ? expected_arg : "(unchecked)"
        );
    }
    return ok;
}

/* Exercises real pathname (glob) expansion for unquoted words containing
 * '*'/'?'/'[...]' (shell_parser.c's shell_parser__words(), backed by
 * shell_glob.c's shell_glob__expand_path()/shell_glob__match()): multiple
 * matches sorted the same way bash's own glob results are, '?' matching
 * exactly one character, a '[...]' bracket class, a pattern that matches
 * nothing left unchanged (nullglob-off, like bash's own default), dotfiles
 * hidden from a wildcard unless the pattern component itself starts with
 * '.', a multi-component pattern (a wildcard directory component followed
 * by a wildcard filename component), an assignment word's own RHS never
 * expanded regardless of quoting, and -- unlike shell_glob__match() as
 * `case`/`esac`'s own pattern matcher, which never touches the filesystem --
 * quoting (single, double, or backslash) suppressing this real, filesystem-
 * touching expansion the way it does in bash, even though the shell's other
 * quoting gaps don't (see shell_glob.h's own doc comment). */
bool selftest__run_shell_glob_case(void) {
    if (!selftest__shell_register_probe()) return false;
    if (!selftest__shell_glob_fixture_setup()) {
        (void)selftest__shell_glob_fixture_teardown();
        return false;
    }
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    bool ok = shell__execute_line(&state, "cd /apps/shell_glob_test_dir") == 0;
    /* '*' matches every non-dotfile entry, sorted. */
    ok = selftest__shell_glob_step(
        ok, &state, "sum=; for f in *.txt; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0,
        "a.txt,b.txt,z.txt,"
    );
    /* '?' matches exactly one character. */
    ok = selftest__shell_glob_step(
        ok, &state, "sum=; for f in ?.txt; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0,
        "a.txt,b.txt,z.txt,"
    );
    /* '[...]' matches only the members actually present. */
    ok = selftest__shell_glob_step(ok, &state, "shell_test_probe [xyz].txt", 0, "z.txt");
    /* A pattern that matches nothing is left unchanged, exactly like
     * nullglob-off bash -- not an error, not an empty word. */
    ok = selftest__shell_glob_step(ok, &state, "shell_test_probe *.nomatch", 0, "*.nomatch");
    /* A wildcard never matches a dotfile's name unless the pattern component
     * itself starts with '.' too. */
    ok = selftest__shell_glob_step(ok, &state, "shell_test_probe *hidden*", 0, "*hidden*");
    ok = selftest__shell_glob_step(ok, &state, "shell_test_probe .*", 0, ".hidden");
    /* A pattern can have a wildcard component that isn't the last one. */
    ok = selftest__shell_glob_step(
        ok, &state, "shell_test_probe /apps/shell_glob_test_dir/s*/inner.txt", 0,
        "/apps/shell_glob_test_dir/sub/inner.txt"
    );
    /* An assignment word's own RHS is never glob-expanded, quoted or not --
     * but a later unquoted reference to that variable still is, exactly like
     * bash's own "pattern='*.txt'; echo $pattern". */
    ok = selftest__shell_glob_step(ok, &state, "pattern=*.txt", 0, NULL);
    ok = selftest__shell_glob_step(ok, &state, "shell_test_probe \"$pattern\"", 0, "*.txt");
    ok = selftest__shell_glob_step(
        ok, &state, "sum=; for f in $pattern; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0,
        "a.txt,b.txt,z.txt,"
    );
    /* Quoting (and backslash-escaping) suppresses real pathname expansion
     * the same way it does in bash, even for a variable whose *value*
     * happens to be nothing but a glob metacharacter -- this is what makes
     * it safe to carry an arbitrary, possibly-corrupted or
     * attacker-influenced string through "$var" without it silently fanning
     * out into multiple words. */
    ok = selftest__shell_glob_step(ok, &state, "v=*", 0, NULL);
    ok = selftest__shell_glob_step(ok, &state, "shell_test_probe \"$v\"", 0, "*");
    ok = selftest__shell_glob_step(ok, &state, "shell_test_probe \\*", 0, "*");
    ok = selftest__shell_glob_step(
        ok, &state, "sum=; for f in $v; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0,
        "a.txt,b.txt,sub,z.txt,"
    );
    ok = ok && shell__execute_line(&state, "cd /") == 0;

    shell__state_free(&state);
    ok = selftest__shell_glob_fixture_teardown() && ok;
    printf("[selftest] shell/glob: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Like selftest__shell_glob_step() above, for selftest__run_shell_brace_case()
 * below. */
static bool selftest__shell_brace_step(
    bool ok_so_far, shell_state_t *state, const char *line, int expected_status, const char *expected_arg
) {
    if (!ok_so_far) return false;
    int status = shell__execute_line(state, line);
    bool ok = status == expected_status && (expected_arg == NULL || strcmp(s_probe_arg, expected_arg) == 0);
    if (!ok) {
        printf(
            "[selftest] shell/brace: `%s` -> status=%d (want %d) arg=\"%s\" (want \"%s\")\n", line, status,
            expected_status, s_probe_arg, expected_arg != NULL ? expected_arg : "(unchecked)"
        );
    }
    return ok;
}

/* Exercises "{a,b,c}"-style brace (comma-list) expansion (shell_parser.c's
 * shell_parser__words(), backed by shell_brace.c): a simple comma-list,
 * sibling groups combining as a cartesian product, a group nested inside
 * another, a "{...}" with no top-level comma left completely literal (unlike
 * a real group, bash doesn't even strip its braces), quoting and
 * backslash-escaping suppressing this the same way they suppress pathname
 * expansion (shell_glob.h), and -- since brace expansion runs strictly
 * before pathname expansion, on each result independently -- a brace group
 * combined with a glob metacharacter in the same word, reusing
 * selftest__run_shell_glob_case()'s own fixture directory. */
bool selftest__run_shell_brace_case(void) {
    if (!selftest__shell_register_probe()) return false;
    if (!selftest__shell_glob_fixture_setup()) {
        (void)selftest__shell_glob_fixture_teardown();
        return false;
    }
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;

    /* A simple comma-list expands to one word per alternative. */
    bool ok = selftest__shell_brace_step(
        true, &state, "sum=; for f in file{1,2,3}.txt; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0,
        "file1.txt,file2.txt,file3.txt,"
    );
    /* Two sibling groups in one word combine as a cartesian product. */
    ok = selftest__shell_brace_step(
        ok, &state, "sum=; for f in {a,b}{1,2}; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0,
        "a1,a2,b1,b2,"
    );
    /* A group can itself contain another group. */
    ok = selftest__shell_brace_step(
        ok, &state, "sum=; for f in {a,{b,c}}; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0, "a,b,c,"
    );
    /* A "{...}" with no top-level comma isn't a group at all -- left
     * completely literal, braces and all, exactly like bash. */
    ok = selftest__shell_brace_step(ok, &state, "shell_test_probe {foo}", 0, "{foo}");
    /* Quoting (and backslash-escaping) suppresses brace expansion the same
     * way it suppresses pathname expansion. */
    ok = selftest__shell_brace_step(ok, &state, "shell_test_probe \"{a,b}\"", 0, "{a,b}");
    ok = selftest__shell_brace_step(ok, &state, "shell_test_probe \\{a,b\\}", 0, "{a,b}");
    /* Brace expansion runs before pathname expansion, on each resulting word
     * independently: one alternative's glob matches real files, the other's
     * matches nothing and is left unchanged. */
    ok = shell__execute_line(&state, "cd /apps/shell_glob_test_dir") == 0 && ok;
    ok = selftest__shell_brace_step(
        ok, &state, "sum=; for f in {sub,doesnotexist}/*.txt; do sum=\"$sum$f,\"; done; shell_test_probe \"$sum\"", 0,
        "sub/inner.txt,doesnotexist/*.txt,"
    );
    ok = ok && shell__execute_line(&state, "cd /") == 0;

    shell__state_free(&state);
    ok = selftest__shell_glob_fixture_teardown() && ok;
    printf("[selftest] shell/brace: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* "producer | consumer >> file" must honor the consumer's own redirection --
 * shell_executor__pipe_to_external()/pipe_write() used to just relay the
 * consumer's output straight to the shell's own stdio (see
 * shell_executor__pipe_relay()) with no regard for target->redirect at all,
 * silently discarding it and leaving the redirect target untouched/empty.
 * Uses "cat"/"head" here in place of a real "wifi scan" (QEMU has no wifi
 * hardware to scan with, but the bug is in the shell's pipe plumbing, not
 * in any particular producer). Runs the same line twice to also confirm
 * ">>" appends rather than truncating on the second run.
 *
 * Captured content isn't compared byte-for-byte under QEMU:
 * shell_executor__buffer_t (used both for the pipe's capture and for writing
 * the redirect target) is backed by memory__external_malloc()/
 * memory__external_memcpy(), and under QEMU there's no emulated PSRAM
 * (CONFIG_SPIRAM is unset in build-qemu/sdkconfig), so the allocation falls
 * through to the swap backend, whose flash-mapped pointer does not reliably
 * reflect memcpy writes -- the same documented glitch bnu_test.c's grep case
 * works around (see the comment there and memory_test.c's own
 * CONFIG_BRUCE_QEMU_TEST_MODE guards). On real hardware PSRAM is available
 * and content is compared exactly, including the '\r' that
 * stdio__session_write_output()'s ONLCR translation inserts before every
 * '\n' relayed through a routed session -- a pre-existing characteristic of
 * the whole capture pipeline (also shared by "cmd > file"), not unique to
 * this fix. */
bool selftest__run_shell_pipe_redirect_case(void) {
    const char *source_path = "/apps/shell_pipe_source.txt";
    const char *result_path = "/apps/shell_pipe_result.txt";
    static const char source_text[] = "line1\nline2\nline3\n";
    (void)storage__remove(source_path);
    (void)storage__remove(result_path);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    if (storage__open(
            source_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, source_text, sizeof(source_text) - 1, &written) != BRUCE_OK ||
        written != sizeof(source_text) - 1 || storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(source_path);
        printf("[selftest] shell/pipe-redirect: could not stage fixture\n");
        return false;
    }

    /* Run each attempt as a spawned "shell -c ..." child with its own routed
     * stdio session -- the same pattern selftest__shell_read_probe() uses --
     * rather than calling shell__execute_line() directly in the selftest's
     * own process: pipe_write()'s target runs as a background child of
     * *whichever* process calls it, and its relay/drain loop expects a real
     * routed session around that call, not the selftest task's own bare
     * (session-less) context. */
    char command[160];
    snprintf(command, sizeof(command), "-c \"cat %s | head -n2 >> %s\"", source_path, result_path);
    int status_a = -1, status_b = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
        int *status_out = attempt == 0 ? &status_a : &status_b;
        if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
            (void)stdio__session_close(session);
            break;
        }
        int launched = app_runner__run("shell", command, BRUCE_LAUNCH_BACKGROUND);
        (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
        if (launched > 0) {
            bruce_process_status_t status;
            if (process__wait_status((bruce_process_id_t)launched, 5000, &status) == BRUCE_OK &&
                status.reason == BRUCE_PROCESS_EXITED) {
                *status_out = status.exit_code;
            }
        }
        (void)stdio__session_close(session);
    }

    char result[64] = {0};
    size_t result_size = 0;
    bruce_result_t read_result = BRUCE_ERR_NOT_FOUND;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_result = storage__read(file, result, sizeof(result) - 1, &result_size);
        (void)storage__close(file);
    }
    (void)storage__remove(source_path);
    (void)storage__remove(result_path);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    bool ok = status_a == 0 && status_b == 0 && read_result == BRUCE_OK && result_size > 0;
#else
    static const char expected[] = "line1\r\nline2\r\nline1\r\nline2\r\n";
    bool ok = status_a == 0 && status_b == 0 && read_result == BRUCE_OK &&
              result_size == sizeof(expected) - 1 && memcmp(result, expected, sizeof(expected) - 1) == 0;
#endif
    if (!ok) {
        printf(
            "[selftest] shell/pipe-redirect: status=%d,%d read=%d size=%u\n", status_a, status_b, read_result,
            (unsigned)result_size
        );
    }
    printf("[selftest] shell/pipe-redirect: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Runs `command` (a "shell -c ..." argument string) as a background child
 * with its own routed stdio session, the same way
 * selftest__run_shell_pipe_redirect_case() above does, and returns its exit
 * code (or -1 if it never got that far). Shared by the tr/tee checks below
 * so each only has to build its own command line and check the resulting
 * file. */
static int selftest__shell_run_pipe_command(const char *command) {
    int status = -1;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) == BRUCE_OK && stdio__session_route_children(session) == BRUCE_OK) {
        int launched = app_runner__run("shell", command, BRUCE_LAUNCH_BACKGROUND);
        (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
        if (launched > 0) {
            bruce_process_status_t proc_status;
            if (process__wait_status((bruce_process_id_t)launched, 5000, &proc_status) == BRUCE_OK &&
                proc_status.reason == BRUCE_PROCESS_EXITED) {
                status = proc_status.exit_code;
            }
        }
    }
    (void)stdio__session_close(session);
    return status;
}

/* Exercises shell_executor__pipe_write()'s handling of a pipe destination
 * that exits (here, a real argument-parse failure) without ever reading its
 * "--stdin-size N" bytes: "ping" (an external, non-builtin app, so the pipe
 * itself is legal -- it just isn't a pipe-aware command and never will be,
 * "--stdin-size" being meaningless for it) is given no host, so it fails
 * ap_parse() and exits in an instant, having read nothing. The producer side
 * (three 80-byte "echo" words, comfortably over STDIO__INPUT_CAPACITY's 256
 * bytes once joined by the pseudo-producer's own separating spaces -- kept
 * to several words under shell_parser.h's own SHELL__WORD_MAX (256) rather
 * than one ~300-byte word, which shell_parser__words() would itself reject
 * as "expanded word too long" before the pipe ever ran at all) guarantees
 * pipe_write()'s input-feeding loop hits BRUCE_ERR_RESOURCE_LIMIT at least
 * once with nothing left to ever drain that ring buffer again -- before the
 * fix this fed loop spun on runtime__delay(1) forever with no liveness check
 * on the destination at all, hanging the whole pipeline (and, transitively,
 * this selftest, since shell__execute_line() runs it synchronously) instead
 * of ever completing. Run as a spawned "shell -c ..." child with a bounded
 * 5000ms wait (same pattern as selftest__run_shell_pipe_redirect_case()
 * above) specifically so a regression back to that hang fails this selftest
 * instead of wedging the whole suite: selftest__shell_run_pipe_command()
 * returns -1 on a timeout, which is what "status == -1" below actually
 * catches. The final "> file" redirect also confirms the fix's other half --
 * that ping's own "error: '--stdin-size' is not a recognised option"
 * diagnostic (ping never calls ap_unknown_options_as_args(), so
 * pipe_write()'s own "--stdin-size N" prefix -- meaningless to a command
 * that was never meant to be a pipe destination -- is rejected outright,
 * before ap_parse() ever gets as far as noticing "host" is missing too) is
 * captured and written out rather than being silently discarded once the
 * destination is known to be gone (see shell_executor__pipe_write()'s
 * "destination_exited" branch). */
bool selftest__run_shell_pipe_early_exit_case(void) {
    const char *result_path = "/apps/shell_pipe_early_exit_result.txt";
    (void)storage__remove(result_path);

    char word[81];
    memset(word, 'A', sizeof(word) - 1);
    word[sizeof(word) - 1] = '\0';

    char command[400];
    snprintf(command, sizeof(command), "-c \"echo %s %s %s | ping > %s\"", word, word, word, result_path);
    int status = selftest__shell_run_pipe_command(command);

    char result[128] = {0};
    size_t result_size = 0;
    bruce_result_t read_result = BRUCE_ERR_NOT_FOUND;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_result = storage__read(file, result, sizeof(result) - 1, &result_size);
        (void)storage__close(file);
    }
    (void)storage__remove(result_path);

    bool ok = status != -1 && status != 0 && read_result == BRUCE_OK &&
              strstr(result, "is not a recognised option") != NULL;
    if (!ok) {
        printf(
            "[selftest] shell/pipe-early-exit: failed (status=%d read=%d result=%.*s)\n", status, (int)read_result,
            (int)result_size, result
        );
    }
    printf("[selftest] shell/pipe-early-exit: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises "cmd > file" / "cmd >> file" for a plain (non-piped, no "<"/
 * heredoc input) external command -- shell_executor__stream_external_to_file()
 * in shell_executor.c, which writes each output chunk straight to the file
 * as the command runs rather than buffering it in memory__external_malloc()-
 * backed storage first. Unlike selftest__run_shell_bnu_text_pipe_case()'s own
 * "|"/">>"-redirected-pipe-destination checks, this path never touches that
 * buffer at all, so -- unlike that test -- content is checked exactly
 * regardless of CONFIG_BRUCE_QEMU_TEST_MODE; nothing here is subject to that
 * backing store's QEMU swap-backend unreliability. Calls shell__execute_line()
 * directly (no spawned "shell -c ..." child, unlike the pipe-backed tests
 * further up) since shell_executor__stream_external_to_file() -- like
 * shell_executor__capture_external() it's modeled on -- creates and owns its
 * own stdio session and never touches the calling task's own (here, the
 * selftest task's bare, session-less) stdio directly. */
bool selftest__run_shell_output_redirect_case(void) {
    const char *source_path = "/apps/shell_output_redirect_source.txt";
    const char *result_path = "/apps/shell_output_redirect_result.txt";
    static const char source_text[] = "alpha\nbeta\n";
    (void)storage__remove(source_path);
    (void)storage__remove(result_path);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    if (storage__open(
            source_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, source_text, sizeof(source_text) - 1, &written) != BRUCE_OK ||
        written != sizeof(source_text) - 1 || storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(source_path);
        printf("[selftest] shell/output-redirect: could not stage fixture\n");
        return false;
    }

    shell_state_t state;
    shell__state_init(&state);
    char command[160];
    snprintf(command, sizeof(command), "cat %s > %s", source_path, result_path);
    int status_truncate = shell__execute_line(&state, command);
    snprintf(command, sizeof(command), "cat %s >> %s", source_path, result_path);
    int status_append = shell__execute_line(&state, command);

    /* Snapshot the file right here, before the failing command below
     * overwrites it -- reading it any later would just observe that
     * command's own (empty, see below) result instead of this one's. */
    char after_append[64] = {0};
    size_t after_append_size = 0;
    bruce_result_t read_after_append = BRUCE_ERR_NOT_FOUND;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_after_append = storage__read(file, after_append, sizeof(after_append) - 1, &after_append_size);
        (void)storage__close(file);
    }

    /* A redirected external command that fails still creates/truncates the
     * target file, same as bash -- exercised by redirecting a command name
     * that can't even be launched (rather than one that launches fine and
     * fails internally, e.g. "cat" on a missing path: this shell has no
     * separate stderr, so a launched command's own error text -- like cat's
     * "cat: PATH: error N" -- goes through the same stdout its normal output
     * would and lands in the file same as any other captured output; only a
     * command that never launches at all writes nothing) onto the same
     * result path a third time, and confirming it comes back empty rather
     * than untouched. */
    snprintf(command, sizeof(command), "shell_output_redirect_no_such_command > %s", result_path);
    int status_fail = shell__execute_line(&state, command);
    shell__state_free(&state);

    char after_fail[8] = {0};
    size_t after_fail_size = 1; /* not 0, so a read failure doesn't look like "confirmed empty" below */
    bruce_result_t read_after_fail = BRUCE_ERR_NOT_FOUND;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_after_fail = storage__read(file, after_fail, sizeof(after_fail), &after_fail_size);
        (void)storage__close(file);
    }
    (void)storage__remove(source_path);
    (void)storage__remove(result_path);

    /* "\r\n", not "\n" -- the same routed-session line-ending convention
     * documented on selftest__run_shell_pipe_redirect_case()'s own
     * expected[] applies here too: a child's stdout crosses the same
     * console-style stdio session either way. */
    static const char expected_after_append[] = "alpha\r\nbeta\r\nalpha\r\nbeta\r\n";
    bool ok = status_truncate == 0 && status_append == 0 && status_fail != 0 && read_after_append == BRUCE_OK &&
              after_append_size == sizeof(expected_after_append) - 1 &&
              memcmp(after_append, expected_after_append, sizeof(expected_after_append) - 1) == 0 &&
              read_after_fail == BRUCE_OK && after_fail_size == 0;
    if (!ok) {
        printf(
            "[selftest] shell/output-redirect: status=%d,%d,%d append_size=%u fail_size=%u\n", status_truncate,
            status_append, status_fail, (unsigned)after_append_size, (unsigned)after_fail_size
        );
    }
    printf("[selftest] shell/output-redirect: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises the fd-numbered/combined-stream output redirection spellings
 * ("2>", "2>>", "&>", "2>&1") shell_parser__extract_redirect() accepts on top
 * of plain ">"/">>" -- see its own doc comment for why they all collapse to
 * the same behavior here: BruceOS's stdio model has exactly one output
 * stream per process (no separate stdout/stderr, as
 * selftest__run_shell_output_redirect_case()'s own comment above notes), so
 * "2>file" has nothing different to redirect than "1>file"/">file" do, and a
 * dup target like "2>&1" is already true before it is even written. Reuses
 * selftest__run_shell_output_redirect_case()'s own source-fixture and
 * expected-text conventions. */
bool selftest__run_shell_fd_redirect_case(void) {
    const char *source_path = "/apps/shell_fd_redirect_source.txt";
    const char *result_path = "/apps/shell_fd_redirect_result.txt";
    static const char source_text[] = "alpha\nbeta\n";
    (void)storage__remove(source_path);
    (void)storage__remove(result_path);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    if (storage__open(
            source_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, source_text, sizeof(source_text) - 1, &written) != BRUCE_OK ||
        written != sizeof(source_text) - 1 || storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(source_path);
        printf("[selftest] shell/fd-redirect: could not stage fixture\n");
        return false;
    }

    shell_state_t state;
    shell__state_init(&state);
    char command[160];

    /* "2>" truncates just like ">" would. */
    snprintf(command, sizeof(command), "cat %s 2> %s", source_path, result_path);
    int status_stderr = shell__execute_line(&state, command);
    /* "2>>" appends just like ">>" would. */
    snprintf(command, sizeof(command), "cat %s 2>> %s", source_path, result_path);
    int status_stderr_append = shell__execute_line(&state, command);

    char after_stderr_append[64] = {0};
    size_t after_stderr_append_size = 0;
    bruce_result_t read_after_stderr_append = BRUCE_ERR_NOT_FOUND;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_after_stderr_append =
            storage__read(file, after_stderr_append, sizeof(after_stderr_append) - 1, &after_stderr_append_size);
        (void)storage__close(file);
    }

    /* "&>" truncates the same single stream "2>"/">" already do. */
    snprintf(command, sizeof(command), "cat %s &> %s", source_path, result_path);
    int status_amp = shell__execute_line(&state, command);

    char after_amp[64] = {0};
    size_t after_amp_size = 0;
    bruce_result_t read_after_amp = BRUCE_ERR_NOT_FOUND;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_after_amp = storage__read(file, after_amp, sizeof(after_amp) - 1, &after_amp_size);
        (void)storage__close(file);
    }

    /* "> file 2>&1" -- the common bash idiom for "capture everything into
     * one file" -- the "2>&1" trails the real ">" redirect and, on this
     * single-stream shell, adds nothing beyond confirming what's already
     * true. */
    snprintf(command, sizeof(command), "cat %s > %s 2>&1", source_path, result_path);
    int status_dup = shell__execute_line(&state, command);

    char after_dup[64] = {0};
    size_t after_dup_size = 0;
    bruce_result_t read_after_dup = BRUCE_ERR_NOT_FOUND;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_after_dup = storage__read(file, after_dup, sizeof(after_dup) - 1, &after_dup_size);
        (void)storage__close(file);
    }

    /* "2>&1" alone, with no real (file-target) redirect on the command at
     * all, has nothing to do -- confirm it's accepted rather than rejected
     * as a syntax error, and that it leaves the result file from the
     * previous check untouched. */
    snprintf(command, sizeof(command), "cat %s 2>&1", source_path);
    int status_dup_only = shell__execute_line(&state, command);

    /* Any file descriptor other than 1 or 2 -- or a dup target naming one --
     * is a parse-time syntax error, same as any other malformed redirection:
     * shell_parser__extract_redirect() rejects it before the command is even
     * looked up, so neither of these touches result_path. */
    snprintf(command, sizeof(command), "cat %s 3> %s", source_path, result_path);
    int status_bad_fd = shell__execute_line(&state, command);
    snprintf(command, sizeof(command), "cat %s 2>&3", source_path);
    int status_bad_dup = shell__execute_line(&state, command);

    shell__state_free(&state);
    (void)storage__remove(source_path);
    (void)storage__remove(result_path);

    static const char expected_after_stderr_append[] = "alpha\r\nbeta\r\nalpha\r\nbeta\r\n";
    static const char expected_after_amp[] = "alpha\r\nbeta\r\n";
    static const char expected_after_dup[] = "alpha\r\nbeta\r\n";
    bool ok = status_stderr == 0 && status_stderr_append == 0 && status_amp == 0 && status_dup == 0 &&
              status_dup_only == 0 && status_bad_fd != 0 && status_bad_dup != 0 &&
              read_after_stderr_append == BRUCE_OK &&
              after_stderr_append_size == sizeof(expected_after_stderr_append) - 1 &&
              memcmp(after_stderr_append, expected_after_stderr_append, sizeof(expected_after_stderr_append) - 1) ==
                  0 &&
              read_after_amp == BRUCE_OK && after_amp_size == sizeof(expected_after_amp) - 1 &&
              memcmp(after_amp, expected_after_amp, sizeof(expected_after_amp) - 1) == 0 &&
              read_after_dup == BRUCE_OK && after_dup_size == sizeof(expected_after_dup) - 1 &&
              memcmp(after_dup, expected_after_dup, sizeof(expected_after_dup) - 1) == 0;
    if (!ok) {
        printf(
            "[selftest] shell/fd-redirect: status=%d,%d,%d,%d,%d,%d,%d stderr_append_size=%u amp_size=%u dup_size=%u\n",
            status_stderr, status_stderr_append, status_amp, status_dup, status_dup_only, status_bad_fd,
            status_bad_dup, (unsigned)after_stderr_append_size, (unsigned)after_amp_size, (unsigned)after_dup_size
        );
    }
    printf("[selftest] shell/fd-redirect: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises "builtin > file" / "builtin >> file" / "myfunc > file" --
 * shell_executor__builtin_redirected() in shell_executor.c, which (unlike a
 * real external command) has no separate child process to relay from: it
 * temporarily reroutes the shell's own current session
 * (stdio__session_capture_self()) into a private capture session, runs the
 * builtin/function in-line, then writes whatever got captured to the target
 * file. Called directly via shell__execute_line() on the selftest task, same
 * reasoning as selftest__run_shell_output_redirect_case() above -- the
 * capture session this creates is entirely its own, needing no pre-routed
 * session on the calling task the way a spawned-child test would. Also
 * exercises "<" on a builtin/function -- shell_executor__builtin_with_input()
 * -- alone (a bare "read" builtin), combined with ">" on a plain builtin and
 * on a function whose body reads its own redirected input, a second "read"
 * past the input's one line (proving stdio__session_close_input() delivers a
 * clean end-of-input instead of hanging), and the "target doesn't exist"
 * error path shared with a plain "< file" on any other command. */
bool selftest__run_shell_builtin_redirect_case(void) {
    const char *echo_path = "/apps/shell_builtin_redirect_echo.txt";
    const char *func_path = "/apps/shell_builtin_redirect_func.txt";
    const char *read_in_path = "/apps/shell_builtin_redirect_read_in.txt";
    const char *confirm_path = "/apps/shell_builtin_redirect_confirm.txt";
    const char *combo_path = "/apps/shell_builtin_redirect_combo.txt";
    const char *show_path = "/apps/shell_builtin_redirect_show.txt";
    (void)storage__remove(echo_path);
    (void)storage__remove(func_path);
    (void)storage__remove(read_in_path);
    (void)storage__remove(confirm_path);
    (void)storage__remove(combo_path);
    (void)storage__remove(show_path);

    static const char read_in_text[] = "redirected\n";
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t staged_written = 0;
    bool staged =
        storage__open(
            read_in_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) == BRUCE_OK &&
        storage__write(file, read_in_text, sizeof(read_in_text) - 1, &staged_written) == BRUCE_OK &&
        staged_written == sizeof(read_in_text) - 1 && storage__close(file) == BRUCE_OK;
    if (!staged && file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);

    shell_state_t state;
    shell__state_init(&state);
    char command[160];
    snprintf(command, sizeof(command), "echo hello world > %s", echo_path);
    int status_echo = shell__execute_line(&state, command);
    snprintf(command, sizeof(command), "echo again >> %s", echo_path);
    int status_echo_append = shell__execute_line(&state, command);

    int status_def = shell__execute_line(&state, "greet() { echo hi; echo there; }");
    snprintf(command, sizeof(command), "greet > %s", func_path);
    int status_func = shell__execute_line(&state, command);

    /* Bare "<" on a builtin, no ">" of its own: `read` pulls its line from
     * the redirected file instead of a real stdin, setting $line the same as
     * if it had been typed -- a following, separately-redirected statement
     * then proves that actually happened. */
    snprintf(command, sizeof(command), "read line < %s", read_in_path);
    int status_read = shell__execute_line(&state, command);
    snprintf(command, sizeof(command), "echo \"confirmed:$line\" > %s", confirm_path);
    int status_confirm = shell__execute_line(&state, command);

    /* "<" and ">" together on a plain builtin (not a function): stdio__read_line()
     * echoes each consumed input byte back out (see its own doc comment) the
     * same as if it had been typed at a real console, and since this capture
     * session backs both the input side and the output side at once here,
     * that echo is exactly what ends up in combo_path -- `read` itself prints
     * nothing else. */
    snprintf(command, sizeof(command), "read combo < %s > %s", read_in_path, combo_path);
    int status_combo = shell__execute_line(&state, command);

    /* "<" and ">" together on a function: proves the input side
     * (shell_executor__builtin_with_input()) and the output-capture side
     * (shell_executor__builtin_redirected()'s own mechanism) compose in one
     * call, not just work in isolation. Same echo-then-own-output shape as
     * the plain-builtin combo case above. */
    int status_show_def = shell__execute_line(&state, "showline() { read got; echo \"got:$got\"; }");
    snprintf(command, sizeof(command), "showline < %s > %s", read_in_path, show_path);
    int status_show = shell__execute_line(&state, command);

    /* A second "read" past the one line the redirected input actually has:
     * proves stdio__session_close_input() delivers a clean end-of-input
     * (read's own status 1, $b left unset) instead of the second read
     * blocking forever polling a session nothing will ever add more bytes
     * to -- the exact hang shell_executor__builtin_with_input()'s "always
     * queue everything up front, then close the input side" design exists
     * to avoid. */
    int status_twice_def = shell__execute_line(&state, "twice() { read a; read b; }");
    snprintf(command, sizeof(command), "twice < %s", read_in_path);
    int status_twice = shell__execute_line(&state, command);

    /* A "<" target that doesn't exist is a hard error for a builtin/function
     * exactly like it already is for a plain "< file" or an external
     * command's "<" -- shell_executor__read_file() failing to open it. */
    int status_missing = shell__execute_line(&state, "read x < /apps/shell_builtin_redirect_missing.txt");
    shell__state_free(&state);

    char echo_result[64] = {0};
    size_t echo_size = 0;
    bruce_result_t read_echo = BRUCE_ERR_NOT_FOUND;
    if (storage__open(echo_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_echo = storage__read(file, echo_result, sizeof(echo_result) - 1, &echo_size);
        (void)storage__close(file);
    }
    char func_result[64] = {0};
    size_t func_size = 0;
    bruce_result_t read_func = BRUCE_ERR_NOT_FOUND;
    if (storage__open(func_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_func = storage__read(file, func_result, sizeof(func_result) - 1, &func_size);
        (void)storage__close(file);
    }
    char confirm_result[64] = {0};
    size_t confirm_size = 0;
    bruce_result_t read_confirm = BRUCE_ERR_NOT_FOUND;
    if (storage__open(confirm_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_confirm = storage__read(file, confirm_result, sizeof(confirm_result) - 1, &confirm_size);
        (void)storage__close(file);
    }
    char combo_result[64] = {0};
    size_t combo_size = 0;
    bruce_result_t read_combo = BRUCE_ERR_NOT_FOUND;
    if (storage__open(combo_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_combo = storage__read(file, combo_result, sizeof(combo_result) - 1, &combo_size);
        (void)storage__close(file);
    }
    char show_result[64] = {0};
    size_t show_size = 0;
    bruce_result_t read_show = BRUCE_ERR_NOT_FOUND;
    if (storage__open(show_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_show = storage__read(file, show_result, sizeof(show_result) - 1, &show_size);
        (void)storage__close(file);
    }
    (void)storage__remove(echo_path);
    (void)storage__remove(func_path);
    (void)storage__remove(read_in_path);
    (void)storage__remove(confirm_path);
    (void)storage__remove(combo_path);
    (void)storage__remove(show_path);

    /* "\r\n", not "\n" -- same ONLCR session-output convention noted on
     * selftest__run_shell_output_redirect_case()'s own expected[] above: the
     * capture session this goes through applies it just the same as any
     * other session. expected_combo/expected_show both lead with
     * "redirected\r\n" -- stdio__read_line()'s own per-byte echo of what it
     * consumed from the redirected input, landing in the same captured
     * output as whatever the builtin/function prints afterward (see the
     * comment above the "combo" statement). */
    static const char expected_echo[] = "hello world\r\nagain\r\n";
    static const char expected_func[] = "hi\r\nthere\r\n";
    bool ok = staged && status_echo == 0 && status_echo_append == 0 && status_def == 0 && status_func == 0 &&
              status_read == 0 && status_confirm == 0 && status_combo == 0 && status_show_def == 0 &&
              status_show == 0 && status_twice_def == 0 && status_missing == 1 && read_echo == BRUCE_OK &&
              echo_size == sizeof(expected_echo) - 1 &&
              memcmp(echo_result, expected_echo, sizeof(expected_echo) - 1) == 0 && read_func == BRUCE_OK &&
              func_size == sizeof(expected_func) - 1 &&
              memcmp(func_result, expected_func, sizeof(expected_func) - 1) == 0;
    /* $line/$got/the echoed bytes read_combo and read_show hold all trace
     * back to shell_executor__read_file()'s memory__external_malloc()-backed
     * buffer feeding stdio__session_write_input() -- unreliable under QEMU's
     * swap-backend fallback for the same reason noted on
     * selftest__run_shell_pipe_redirect_case() above, so content there isn't
     * compared byte-for-byte under QEMU. status_twice is unchecked under QEMU
     * for the same root cause: a stray byte among the corrupted-but-correctly-
     * *counted* input can happen to look like '\n'/'\r' to stdio__read_line(),
     * ending "read a" before the true line boundary and leaving "read b" real
     * (if garbled) bytes to read instead of the end-of-input
     * stdio__session_close_input() otherwise guarantees once the queue is
     * actually empty -- the byte *count* fed in (and so exactly when it runs
     * out) is unaffected, only which value each byte holds is. On real
     * hardware PSRAM is available and every one of these is exact, including
     * status_twice == 1. */
#if CONFIG_BRUCE_QEMU_TEST_MODE
    ok = ok && read_confirm == BRUCE_OK && confirm_size > 0 && read_combo == BRUCE_OK && combo_size > 0 &&
         read_show == BRUCE_OK && show_size > 0;
#else
    static const char expected_confirm[] = "confirmed:redirected\r\n";
    static const char expected_combo[] = "redirected\r\n";
    static const char expected_show[] = "redirected\r\ngot:redirected\r\n";
    ok = ok && status_twice == 1 && read_confirm == BRUCE_OK && confirm_size == sizeof(expected_confirm) - 1 &&
         memcmp(confirm_result, expected_confirm, sizeof(expected_confirm) - 1) == 0 && read_combo == BRUCE_OK &&
         combo_size == sizeof(expected_combo) - 1 &&
         memcmp(combo_result, expected_combo, sizeof(expected_combo) - 1) == 0 && read_show == BRUCE_OK &&
         show_size == sizeof(expected_show) - 1 && memcmp(show_result, expected_show, sizeof(expected_show) - 1) == 0;
#endif
    if (!ok) {
        printf(
            "[selftest] shell/builtin-redirect: status=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d staged=%d echo=%d/%u "
            "func=%d/%u confirm=%d/%u combo=%d/%u show=%d/%u\n",
            status_echo, status_echo_append, status_def, status_func, status_read, status_confirm, status_combo,
            status_show_def, status_show, status_twice_def, status_twice, status_missing, staged, read_echo,
            (unsigned)echo_size, read_func, (unsigned)func_size, read_confirm, (unsigned)confirm_size, read_combo,
            (unsigned)combo_size, read_show, (unsigned)show_size
        );
    }
    printf("[selftest] shell/builtin-redirect: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises "cmd < file" -- shell_executor__load_redirect_input() in
 * shell_executor.c, which reads `file` in full and feeds it to the command's
 * stdin via shell_executor__pipe_write() (the same "--stdin-size N"-fed
 * mechanism a "|" pipe destination already uses). The one content check runs
 * as a spawned "shell -c ..." child with its own routed stdio session, the
 * same pattern (and for the same reason -- see its own doc comment)
 * selftest__run_shell_pipe_redirect_case() uses, and combines "<" with ">" so
 * the result lands in a file this can read back rather than needing to
 * capture a live relay; the parse/dispatch-rejection checks below it need no
 * such session since they never reach shell_executor__pipe_write() at all. */
bool selftest__run_shell_input_redirect_case(void) {
    const char *source_path = "/apps/shell_input_redirect_source.txt";
    const char *result_path = "/apps/shell_input_redirect_result.txt";
    static const char source_text[] = "line1\nline2\nline3\n";
    (void)storage__remove(source_path);
    (void)storage__remove(result_path);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    if (storage__open(
            source_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, source_text, sizeof(source_text) - 1, &written) != BRUCE_OK ||
        written != sizeof(source_text) - 1 || storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(source_path);
        printf("[selftest] shell/input-redirect: could not stage fixture\n");
        return false;
    }

    char command[160];
    snprintf(command, sizeof(command), "-c \"head -n2 < %s > %s\"", source_path, result_path);
    int status = selftest__shell_run_pipe_command(command);

    char result[64] = {0};
    size_t result_size = 0;
    bruce_result_t read_result = BRUCE_ERR_NOT_FOUND;
    if (storage__open(result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_result = storage__read(file, result, sizeof(result) - 1, &result_size);
        (void)storage__close(file);
    }
    (void)storage__remove(result_path);

    shell_state_t state;
    shell__state_init(&state);
    bool rejections =
        /* No such file -- caught before any process launch, so this doesn't
         * need a routed session either. */
        shell__execute_line(&state, "cat < /apps/shell_input_redirect_missing.txt") == 1 &&
        /* A builtin's "<" target missing is the same class of error --
         * shell_executor__load_redirect_input() fails to open it via
         * shell_executor__read_file() before "echo" ever runs (see
         * selftest__run_shell_builtin_redirect_case() for the
         * target-exists path, on both a builtin and a function). */
        shell__execute_line(&state, "echo hi < /apps/shell_input_redirect_missing.txt") == 1 &&
        /* At most one "<" per command, same as ">" (SHELL__WORD_MAX etc.
         * aside, this is a pure parse-time check -- no file even needs to
         * exist for it to fire). */
        shell__execute_line(&state, "cat < a < b") == 2 &&
        /* "<" and a heredoc marker both claim the same command's stdin. */
        shell__execute_line(&state, "cat < a <<EOF") == 2;
    shell__state_free(&state);
    (void)storage__remove(source_path);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    bool ok = status == 0 && read_result == BRUCE_OK && result_size > 0 && rejections;
#else
    static const char expected[] = "line1\r\nline2\r\n";
    bool ok = status == 0 && read_result == BRUCE_OK && result_size == sizeof(expected) - 1 &&
              memcmp(result, expected, sizeof(expected) - 1) == 0 && rejections;
#endif
    if (!ok) {
        printf(
            "[selftest] shell/input-redirect: status=%d read=%d size=%u rejections=%d\n", status, read_result,
            (unsigned)result_size, rejections
        );
    }
    printf("[selftest] shell/input-redirect: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises ">"/">>"/"<" redirection on a standalone "((...))" statement
 * (shell_executor__command()'s arithmetic branch in shell_executor.c):
 * "((...))" reads no stdin and writes no stdout of its own, so a ">"/">>"
 * target is just created/truncated (or left unappended-to) the same way a
 * bare "> file" with no command at all already is, and a "<" target is only
 * ever validated -- shell_executor__probe_input_target() -- never actually
 * read from. Also confirms an arithmetic *error* skips the output target
 * entirely (no truncate-then-fail): the error is reported before
 * shell_executor__resolve_redirect_target()/storage__open() ever run. */
bool selftest__run_shell_arith_redirect_case(void) {
    const char *out_path = "/apps/shell_arith_redirect_out.txt";
    const char *err_path = "/apps/shell_arith_redirect_err.txt";
    const char *in_path = "/apps/shell_arith_redirect_in.txt";
    (void)storage__remove(out_path);
    (void)storage__remove(err_path);
    (void)storage__remove(in_path);

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    static const char in_text[] = "unused\n";
    bool staged =
        storage__open(in_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file) ==
            BRUCE_OK &&
        storage__write(file, in_text, sizeof(in_text) - 1, &written) == BRUCE_OK && written == sizeof(in_text) - 1 &&
        storage__close(file) == BRUCE_OK;
    if (!staged && file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);

    shell_state_t state;
    shell__state_init(&state);
    char command[160];
    /* Truthy result (5 != 0): status 0, target created/truncated empty. */
    snprintf(command, sizeof(command), "((x = 5)) > %s", out_path);
    int status_truncate = shell__execute_line(&state, command);
    /* Falsy result (0): status 1, ">>" still leaves the (already-empty)
     * target in place rather than skipping it. */
    snprintf(command, sizeof(command), "((x = x - 5)) >> %s", out_path);
    int status_append = shell__execute_line(&state, command);
    /* A genuine arithmetic error must not touch the output target at all. */
    snprintf(command, sizeof(command), "((1 / 0)) > %s", err_path);
    int status_error = shell__execute_line(&state, command);
    /* "<" on an existing target only validates it opens; the arithmetic
     * result is unaffected by its content. */
    snprintf(command, sizeof(command), "((y = 3)) < %s", in_path);
    int status_input = staged ? shell__execute_line(&state, command) : -1;
    /* "<" on a missing target is a hard error, same as everywhere else. */
    int status_input_missing = shell__execute_line(&state, "((z = 1)) < /apps/shell_arith_redirect_missing.txt");
    shell__state_free(&state);

    size_t out_size = 0;
    bruce_result_t read_out = BRUCE_ERR_NOT_FOUND;
    char out_result[8] = {0};
    if (storage__open(out_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_out = storage__read(file, out_result, sizeof(out_result), &out_size);
        (void)storage__close(file);
    }
    bruce_file_id_t err_probe = BRUCE_FILE_ID_INVALID;
    bool err_created = storage__open(err_path, BRUCE_STORAGE_OPEN_READ, &err_probe) == BRUCE_OK;
    if (err_created) (void)storage__close(err_probe);
    (void)storage__remove(out_path);
    (void)storage__remove(err_path);
    (void)storage__remove(in_path);

    bool ok = staged && status_truncate == 0 && status_append == 1 && status_error == 2 && status_input == 0 &&
              status_input_missing == 2 && read_out == BRUCE_OK && out_size == 0 && !err_created;
    if (!ok) {
        printf(
            "[selftest] shell/arith-redirect: status=%d,%d,%d,%d,%d staged=%d out=%d/%u err_created=%d\n",
            status_truncate, status_append, status_error, status_input, status_input_missing, staged, read_out,
            (unsigned)out_size, err_created
        );
    }
    printf("[selftest] shell/arith-redirect: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises "<<DELIM"/"<<-DELIM"/"<<'DELIM'" heredocs, script-file-only per
 * this feature's scope (see shell_app.c's SHELL_APP__MAX_HEREDOCS doc
 * comment): a script staged at `script_path` covers an unquoted delimiter
 * (body gets $expanded), a single-quoted one (body stays literal), and
 * "<<-" (leading tabs stripped from both the body and the terminator line),
 * each redirecting the heredoc'd "cat"'s output to its own result file so
 * this can read it back afterward -- run as a spawned "shell <path>" child
 * with a routed session, the same reasoning as
 * selftest__run_shell_input_redirect_case() above. "cat" reads the heredoc
 * body via its "--stdin-size" fallback (see bnu__cat_stdin() in
 * bnu_fs_app.c), the same as the other bnu text tools a piped/redirected
 * source can target. Each heredoc here is also combined with ">", the one
 * case that still buffers instead of streaming (see
 * shell_executor__external_with_input()'s own doc comment), so content is
 * checked under the same CONFIG_BRUCE_QEMU_TEST_MODE relaxation the other
 * memory__external_malloc()-backed-capture tests already use. */
bool selftest__run_shell_heredoc_case(void) {
    const char *script_path = "/apps/shell_heredoc_script.sh";
    const char *expand_path = "/apps/shell_heredoc_result_expand.txt";
    const char *literal_path = "/apps/shell_heredoc_result_literal.txt";
    const char *striptabs_path = "/apps/shell_heredoc_result_striptabs.txt";
    (void)storage__remove(script_path);
    (void)storage__remove(expand_path);
    (void)storage__remove(literal_path);
    (void)storage__remove(striptabs_path);

    char script[512];
    snprintf(
        script, sizeof(script),
        "x=world\n"
        "cat <<EOF > %s\n"
        "hello $x\n"
        "EOF\n"
        "cat <<'EOF' > %s\n"
        "literal $x\n"
        "EOF\n"
        "cat <<-EOF > %s\n"
        "\ttabbed\n"
        "\tEOF\n",
        expand_path, literal_path, striptabs_path
    );
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    size_t script_length = strlen(script);
    if (storage__open(
            script_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK ||
        storage__write(file, script, script_length, &written) != BRUCE_OK || written != script_length ||
        storage__close(file) != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
        (void)storage__remove(script_path);
        printf("[selftest] shell/heredoc: could not stage fixture\n");
        return false;
    }

    int status = -1;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) == BRUCE_OK && stdio__session_route_children(session) == BRUCE_OK) {
        int launched = app_runner__run("shell", script_path, BRUCE_LAUNCH_BACKGROUND);
        (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
        if (launched > 0) {
            bruce_process_status_t proc_status;
            if (process__wait_status((bruce_process_id_t)launched, 5000, &proc_status) == BRUCE_OK &&
                proc_status.reason == BRUCE_PROCESS_EXITED) {
                status = proc_status.exit_code;
            }
        }
    }
    (void)stdio__session_close(session);
    (void)storage__remove(script_path);

    char expand_result[32] = {0};
    size_t expand_size = 0;
    bruce_result_t read_expand = BRUCE_ERR_NOT_FOUND;
    if (storage__open(expand_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_expand = storage__read(file, expand_result, sizeof(expand_result) - 1, &expand_size);
        (void)storage__close(file);
    }
    char literal_result[32] = {0};
    size_t literal_size = 0;
    bruce_result_t read_literal = BRUCE_ERR_NOT_FOUND;
    if (storage__open(literal_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_literal = storage__read(file, literal_result, sizeof(literal_result) - 1, &literal_size);
        (void)storage__close(file);
    }
    char striptabs_result[32] = {0};
    size_t striptabs_size = 0;
    bruce_result_t read_striptabs = BRUCE_ERR_NOT_FOUND;
    if (storage__open(striptabs_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_striptabs = storage__read(file, striptabs_result, sizeof(striptabs_result) - 1, &striptabs_size);
        (void)storage__close(file);
    }
    (void)storage__remove(expand_path);
    (void)storage__remove(literal_path);
    (void)storage__remove(striptabs_path);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    bool ok = status == 0 && read_expand == BRUCE_OK && expand_size > 0 && read_literal == BRUCE_OK &&
              literal_size > 0 && read_striptabs == BRUCE_OK && striptabs_size > 0;
#else
    static const char expected_expand[] = "hello world\r\n";
    static const char expected_literal[] = "literal $x\r\n";
    static const char expected_striptabs[] = "tabbed\r\n";
    bool ok = status == 0 && read_expand == BRUCE_OK && expand_size == sizeof(expected_expand) - 1 &&
              memcmp(expand_result, expected_expand, sizeof(expected_expand) - 1) == 0 && read_literal == BRUCE_OK &&
              literal_size == sizeof(expected_literal) - 1 &&
              memcmp(literal_result, expected_literal, sizeof(expected_literal) - 1) == 0 &&
              read_striptabs == BRUCE_OK && striptabs_size == sizeof(expected_striptabs) - 1 &&
              memcmp(striptabs_result, expected_striptabs, sizeof(expected_striptabs) - 1) == 0;
#endif
    if (!ok) {
        printf(
            "[selftest] shell/heredoc: status=%d expand=%d/%u literal=%d/%u striptabs=%d/%u\n", status, read_expand,
            (unsigned)expand_size, read_literal, (unsigned)literal_size, read_striptabs, (unsigned)striptabs_size
        );
    }
    printf("[selftest] shell/heredoc: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Exercises bnu_cat_app_main()'s interactive fallback (bnu__cat_interactive()
 * in bnu_fs_app.c) and, underneath it, stdio__read_line()'s new Ctrl+D
 * handling -- neither is reached by the heredoc/redirect cases above, which
 * only ever exercise the known-byte-count "--stdin-size" path. Launches
 * "cat" directly (no shell involved -- bare, no file, no "--stdin-size")
 * with its stdio routed to a session this test controls, the same way the
 * heredoc case's spawned "shell" child inherits its session; feeds two typed
 * lines and a Ctrl+D (0x04) on an empty line into the session's input queue,
 * then checks the output matches what a real terminal running bare "cat"
 * shows: each typed line echoed back (stdio__read_line()'s per-character
 * echo), then cat's own re-print of that same line, then a bare newline for
 * the Ctrl+D itself (see stdio__read_line()'s doc comment on why that final
 * "\n" is unconditional). */
bool selftest__run_shell_cat_interactive_case(void) {
    int status = -1;
    char output[128] = {0};
    size_t output_size = 0;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) == BRUCE_OK && stdio__session_route_children(session) == BRUCE_OK) {
        int launched = app_runner__run("cat", "", BRUCE_LAUNCH_BACKGROUND);
        (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
        if (launched > 0) {
            static const char input[] = "line one\nline two\n\x04";
            (void)stdio__session_write_input(session, input, sizeof(input) - 1);
            bruce_process_status_t proc_status;
            if (process__wait_status((bruce_process_id_t)launched, 5000, &proc_status) == BRUCE_OK &&
                proc_status.reason == BRUCE_PROCESS_EXITED) {
                status = proc_status.exit_code;
            }
            for (;;) {
                size_t chunk_size = 0;
                if (stdio__session_read_output(
                        session, output + output_size, sizeof(output) - 1 - output_size, &chunk_size
                    ) != BRUCE_OK ||
                    chunk_size == 0)
                    break;
                output_size += chunk_size;
            }
        }
    }
    (void)stdio__session_close(session);

    static const char expected[] = "line one\r\nline one\r\nline two\r\nline two\r\n\r\n";
    bool ok =
        status == 0 && output_size == sizeof(expected) - 1 && memcmp(output, expected, sizeof(expected) - 1) == 0;
    if (!ok) {
        printf(
            "[selftest] shell/cat_interactive: status=%d output_size=%u output=%.*s\n", status,
            (unsigned)output_size, (int)output_size, output
        );
    }
    printf("[selftest] shell/cat_interactive: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* tr and tee (see bnu_text_app.c) have no file-argument mode at all -- like
 * real tr/tee, they only ever read piped stdin -- so unlike bnu_test.c's
 * other bnu_*_app_main() fixtures, calling them directly with a
 * "--stdin-size N" argv but no routed session behind it would have nothing
 * to read from. This exercises them the same way
 * selftest__run_shell_pipe_redirect_case() above exercises "cat | head": a
 * real "shell -c ..." pipe, with the same QEMU pipe-buffer caveat that
 * comment documents (shell_executor's pipe buffer is
 * memory__external_malloc()-backed, unreliable under QEMU's swap-backend
 * fallback). One three-stage "echo | tr | tee" pipeline exercises
 * shell_executor__pipeline()'s chained-pipe support directly: tr sits in the
 * middle, both consuming echo's captured output and producing its own for
 * tee, which is the final destination and writes what it read to
 * `tee_result_path` (as well as relaying it live, which this doesn't check). */
bool selftest__run_shell_bnu_text_pipe_case(void) {
    const char *tee_result_path = "/apps/shell_bnu_tee_result.txt";
    (void)storage__remove(tee_result_path);

    char command[160];
    snprintf(command, sizeof(command), "-c \"echo abc | tr a-z A-Z | tee %s\"", tee_result_path);
    int status = selftest__shell_run_pipe_command(command);

    char result[32] = {0};
    size_t size = 0;
    bruce_result_t read_result = BRUCE_ERR_NOT_FOUND;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(tee_result_path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_result = storage__read(file, result, sizeof(result) - 1, &size);
        (void)storage__close(file);
    }
    (void)storage__remove(tee_result_path);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    bool ok = status == 0 && read_result == BRUCE_OK && size > 0;
#else
    static const char expected[] = "ABC\n";
    bool ok = status == 0 && read_result == BRUCE_OK && size == sizeof(expected) - 1 &&
              memcmp(result, expected, sizeof(expected) - 1) == 0;
#endif
    if (!ok) {
        printf("[selftest] shell/bnu-text-pipe: status=%d size=%u\n", status, (unsigned)size);
    }
    printf("[selftest] shell/bnu-text-pipe: %s\n", ok ? "OK" : "failed");
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

/* Copies a terminal_grid_t row's glyphs into a NUL-terminated string,
 * trimming trailing spaces -- same helper terminal_test.c uses to inspect a
 * grid after terminal_grid__feed(), duplicated here (it's file-static
 * there) since this is the one shell test that needs to look past the raw
 * bytes the shell wrote into what they'd actually render as. */
static void
selftest__shell_terminal_row_text(const terminal_grid_t *grid, uint16_t row, char *out, size_t out_size) {
    const terminal_cell_t *cells = terminal_grid__active_cells(grid);
    const terminal_cell_t *cell_row = cells + (size_t)row * grid->columns;
    size_t used = 0;
    for (uint16_t x = 0; x < grid->columns && used + 4 < out_size; ++x) {
        const terminal_cell_t *cell = &cell_row[x];
        if (cell->utf8_len == 0) {
            out[used++] = ' ';
        } else {
            memcpy(out + used, cell->utf8, cell->utf8_len);
            used += cell->utf8_len;
        }
    }
    while (used > 0 && out[used - 1] == ' ') used--;
    out[used] = '\0';
}

/* "When a prompt command is too long and it wraps, every new character
 * removes the lines in the terminal" (reported against real hardware).
 * shell_console__clear_previous() figures out how many rows to move up and
 * erase purely from character counts (prompt_width + cursor/line_length,
 * divided by the column count) -- it never learns the terminal's row count,
 * so it has no way to notice when the wrapped line has grown tall enough
 * that returning to "the prompt's first row" would require moving above row
 * 0. CSI A/B (cursor up/down) silently clamp at the screen edges instead of
 * erroring (see terminal_grid__finish_csi()'s 'A'/'B' cases and
 * terminal_grid__clamp_cursor()), and terminal_grid_t keeps no scrollback
 * (see terminal_ansi.h) -- both true of the real on-device renderer, since
 * it's the same terminal_grid_t/terminal_app.c pair for the local shell and
 * the ssh client. This types a long command a byte at a time (matching the
 * user's report) into a terminal too short to hold it all, captures every
 * raw byte the shell writes back, and replays it through that same grid
 * engine to check the screen is still laid out correctly afterward instead
 * of trusting shell_console.c's own row bookkeeping. */
bool selftest__run_shell_prompt_wrap_scroll_case(void) {
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
        return false;
    }
    const uint16_t columns = 10;
    const uint16_t rows = 3;
    bool ok = tty__set_size(session, columns, rows) == BRUCE_OK;

    shell_console__reset_ready();
    int launched = ok ? app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND) : 0;
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    ok = ok && launched > 0;
    if (!ok) {
        (void)stdio__session_close(session);
        return false;
    }
    bruce_process_id_t shell_id = (bruce_process_id_t)launched;
    uint64_t started = runtime__now();
    while (!shell_console__is_ready() && runtime__now() - started < 1000) (void)runtime__delay(5);
    ok = shell_console__is_ready();

    /* Distinct (non-repeating) characters, long enough to overflow a 10x3
     * (30-cell) screen several times over once "bruce$ " is counted in --
     * a missing or duplicated row is easy to spot against a known sequence. */
    static const char typed[] = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRST";
    /* Every keystroke's redraw clears and reprints the whole (growing)
     * prompt+line, so total traffic grows O(typed^2), not O(typed) -- 4096
     * turned out to be too small (it filled solid, at which point
     * stdio__session_push_output_byte()'s backpressure blocks the shell task
     * mid-redraw until this buffer is drained, so a too-small cap doesn't
     * just truncate the capture, it truncates the shell's OWN output right
     * along with it, hiding whatever the last few keystrokes actually drew).
     * Heap-allocated, not a local array -- 16KB would blow through whatever
     * task stack runs a selftest case. */
    const size_t captured_capacity = 16384;
    char *captured = memory__malloc(captured_capacity);
    if (captured == NULL) {
        (void)stdio__session_close(session);
        return false;
    }
    memset(captured, 0, captured_capacity);
    size_t captured_size = 0;
    for (size_t i = 0; ok && i < sizeof(typed) - 1; ++i) {
        ok = stdio__session_write_input(session, &typed[i], 1) == BRUCE_OK;
        (void)runtime__delay(10);
        if (captured_size < captured_capacity - 1) {
            size_t chunk_size = 0;
            (void)stdio__session_read_output(
                session, captured + captured_size, captured_capacity - 1 - captured_size, &chunk_size
            );
            captured_size += chunk_size;
        }
    }
    /* The loop above reads once per keystroke shortly after writing it --
     * fine early on, but each redraw clears and reprints the *whole*
     * (growing) prompt+line, so later keystrokes emit far more bytes than
     * earlier ones, and nothing guarantees the shell task has caught up and
     * finished landing a redraw in the output ring buffer by the time that
     * single read fires. A too-impatient drain here doesn't fail loudly --
     * it silently truncates `captured` mid-redraw, which then LOOKS exactly
     * like the erased/misrendered-row bug this test exists to catch (fewer
     * rows/columns than expected) even though the real terminal_grid replay
     * of the full, untruncated stream is correct. Require a real stretch of
     * silence (20 reads, 10ms apart, all empty) before concluding the shell
     * is done, not just a couple of empty reads. */
    for (int consecutive_empty = 0; ok && consecutive_empty < 20 && captured_size < captured_capacity - 1;) {
        (void)runtime__delay(10);
        size_t chunk_size = 0;
        (void)stdio__session_read_output(
            session, captured + captured_size, captured_capacity - 1 - captured_size, &chunk_size
        );
        captured_size += chunk_size;
        consecutive_empty = chunk_size == 0 ? consecutive_empty + 1 : 0;
    }

    /* Judge the screen right where it stands after the last keystroke --
     * before touching the session again to shut the shell down cleanly, so
     * that cleanup traffic can't get mixed into what's being checked. */
    terminal_cell_t cells[10 * 3];
    terminal_cell_t alt_cells[10 * 3];
    terminal_grid_t grid;
    terminal_grid__init(&grid, cells, alt_cells, columns, rows);
    if (ok) terminal_grid__feed(&grid, captured, captured_size);

    /* selftest__shell_terminal_row_text()'s bound check reserves room for a
     * worst-case 4-byte UTF-8 glyph before writing each cell (`used + 4 <
     * out_size`), so the buffer must fit columns*4+1, not just columns+1 --
     * an 11-byte buffer for a 10-column row stops after ~6 cells no matter
     * what the row actually holds, which read as truncated/erased content
     * and was mistaken for a shell_console.c bug when it was really just
     * this buffer being sized for content, not for the helper's margin. */
    char row_text[3][41] = {{0}}; /* columns(10) * 4-byte-glyph margin + 1 */
    for (uint16_t row = 0; row < rows; ++row) {
        selftest__shell_terminal_row_text(&grid, row, row_text[row], sizeof(row_text[row]));
    }

    /* Work out what a *correctly* redrawn 10x3, no-scrollback terminal must
     * show once the cursor sits at the end of the line: since the whole
     * "bruce$ " + typed stream is longer than the screen, only its tail
     * survives, wrapped into whole-column rows -- exactly as if it had all
     * been printed in one shot rather than one keystroke (and one
     * clear+reprint) at a time. Any row that doesn't match this is either a
     * row the broken math erased and the following reprint never refilled,
     * or one that's showing stale/misplaced content instead. */
    char stream[8 + sizeof(typed)];
    snprintf(stream, sizeof(stream), "bruce$ %s", typed);
    size_t stream_len = strlen(stream);
    size_t last_row_start = ((stream_len - 1) / columns) * columns;
    char expected_row2[11] = {0};
    memcpy(expected_row2, stream + last_row_start, stream_len - last_row_start);
    char expected_row1[11] = {0};
    memcpy(expected_row1, stream + (last_row_start - columns), columns);
    char expected_row0[11] = {0};
    memcpy(expected_row0, stream + (last_row_start - 2 * columns), columns);

    ok = ok && strcmp(row_text[0], expected_row0) == 0 && strcmp(row_text[1], expected_row1) == 0 &&
         strcmp(row_text[2], expected_row2) == 0;
    if (!ok) {
        printf(
            "[selftest] shell/prompt-wrap-scroll: got row0=\"%s\" row1=\"%s\" row2=\"%s\" want "
            "row0=\"%s\" row1=\"%s\" row2=\"%s\"\n",
            row_text[0], row_text[1], row_text[2], expected_row0, expected_row1, expected_row2
        );
        printf("[selftest] shell/prompt-wrap-scroll: captured %u bytes (hex):\n", (unsigned)captured_size);
        for (size_t i = 0; i < captured_size; ++i) {
            printf("%02x ", (unsigned char)captured[i]);
            if (i % 24 == 23) putchar('\n');
        }
        putchar('\n');
    }

    (void)process__signal(shell_id, BRUCE_PROCESS_SIGNAL_INT);
    (void)runtime__delay(50);
    static const char exit_line[] = "exit\n";
    (void)stdio__session_write_input(session, exit_line, strlen(exit_line));
    bruce_process_status_t status;
    (void)process__wait_status(shell_id, 2000, &status);
    (void)stdio__session_close(session);
    memory__free(captured);

    printf("[selftest] shell/prompt-wrap-scroll: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Ctrl+C at the prompt (process__signal(INT), the same call terminal_app.c
 * makes) should throw away the half-typed line and keep the shell running --
 * not exit it, like bash. Uses the same "-i" + stdio__session_* launch shape
 * as selftest__run_terminal_stdio_cancel_case(). */
bool selftest__run_shell_interrupt_case(void) {
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
        return false;
    }
    shell_console__reset_ready();
    int launched = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0) {
        (void)stdio__session_close(session);
        return false;
    }
    bruce_process_id_t shell_id = (bruce_process_id_t)launched;
    uint64_t started = runtime__now();
    while (!shell_console__is_ready() && runtime__now() - started < 1000) (void)runtime__delay(5);

    static const char half_line[] = "echo should-not-run";
    bool ok = shell_console__is_ready() &&
              stdio__session_write_input(session, half_line, strlen(half_line)) == BRUCE_OK;
    if (ok) (void)runtime__delay(50);
    ok = ok && process__signal(shell_id, BRUCE_PROCESS_SIGNAL_INT) == BRUCE_OK;
    if (ok) (void)runtime__delay(100);

    /* Still running -- INT aborted the line, it didn't exit the shell. */
    bruce_process_status_t status;
    ok = ok && process__wait_status(shell_id, 0, &status) == BRUCE_ERR_TIMEOUT;

    /* And the prompt still works afterward. */
    static const char next_line[] = "echo shell-recovered\nexit\n";
    ok = ok && stdio__session_write_input(session, next_line, strlen(next_line)) == BRUCE_OK;
    ok = ok && process__wait_status(shell_id, 2000, &status) == BRUCE_OK &&
         status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0;

    /* 1024 to match STDIO__OUTPUT_CAPACITY -- the shell redraws the whole
     * prompt+line on every keystroke, so a handful of echoed characters
     * easily outgrows a smaller capture buffer before "shell-recovered"
     * ever shows up in it. */
    char output[1024] = {0};
    size_t output_size = 0;
    (void)stdio__session_read_output(session, output, sizeof(output) - 1, &output_size);
    (void)stdio__session_close(session);

    ok = ok && strstr(output, "^C") != NULL && strstr(output, "should-not-run") == NULL &&
         strstr(output, "shell-recovered") != NULL;
    if (!ok) printf("[selftest] shell/interrupt: output=%s\n", output);
    printf("[selftest] shell/interrupt: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_eof_case(void) {
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
        return false;
    }
    shell_console__reset_ready();
    int launched = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0) {
        (void)stdio__session_close(session);
        return false;
    }
    bruce_process_id_t shell_id = (bruce_process_id_t)launched;
    uint64_t started = runtime__now();
    while (!shell_console__is_ready() && runtime__now() - started < 1000) (void)runtime__delay(5);

    /* On a non-empty line, Ctrl+D just deletes under the cursor -- move the
     * cursor to the start with Ctrl+A first so it eats the stray leading 'e'
     * instead of ending the shell. */
    static const char line[] = "eecho hello\x01\x04\n";
    bool ok = shell_console__is_ready() &&
              stdio__session_write_input(session, line, strlen(line)) == BRUCE_OK;
    if (ok) (void)runtime__delay(100);

    bruce_process_status_t status;
    ok = ok && process__wait_status(shell_id, 0, &status) == BRUCE_ERR_TIMEOUT;

    /* Ctrl+D on an empty prompt is end-of-input and exits the shell, with
     * whatever status the last command left behind. */
    static const char eof[] = {0x04};
    ok = ok && stdio__session_write_input(session, eof, sizeof(eof)) == BRUCE_OK;
    ok = ok && process__wait_status(shell_id, 2000, &status) == BRUCE_OK &&
         status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0;

    char output[512] = {0};
    size_t output_size = 0;
    (void)stdio__session_read_output(session, output, sizeof(output) - 1, &output_size);
    (void)stdio__session_close(session);

    ok = ok && strstr(output, "hello") != NULL;
    if (!ok) printf("[selftest] shell/eof: output=%s\n", output);
    printf("[selftest] shell/eof: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Bash-style "cmd \" line continuation, and the history-flattening this
 * shell app.c gained for it (shell_app__history_flatten()): a backslash-
 * continued command and a multi-line if/then/fi block must each run as one
 * logical command AND land in history as exactly one entry, not one per
 * physical line -- shell_history.c's file format is strictly one line per
 * entry, so a real embedded '\n' would read back as several unrelated
 * commands instead of the one that was actually typed. */
bool selftest__run_shell_history_multiline_case(void) {
    if (!selftest__shell_register_probe()) return false;
    (void)storage__remove(SHELL_HISTORY_PATH);
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
        return false;
    }
    shell_console__reset_ready();
    int launched = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0) {
        (void)stdio__session_close(session);
        return false;
    }
    bruce_process_id_t shell_id = (bruce_process_id_t)launched;
    uint64_t started = runtime__now();
    while (!shell_console__is_ready() && runtime__now() - started < 1000) (void)runtime__delay(5);
    s_probe_calls = 0;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));

    /* Verified via shell_test_probe's call count/argument rather than the
     * session's raw captured text, for two reasons: that capture is a small
     * fixed-size window (see the interrupt-case comment on
     * STDIO__OUTPUT_CAPACITY) a longer interaction like this one would
     * otherwise fill; and more importantly, stdio__session_push_output_byte()
     * *blocks* (busy-waits) once that buffer is full rather than dropping
     * bytes, so a child that keeps echoing keystrokes without anyone ever
     * draining its session's output would just hang forever mid-command.
     * The polling loops below call stdio__session_read_output() on every
     * spin specifically to keep that buffer drained throughout, not only
     * to notice s_probe_calls sooner.
     *
     * "shell_test_probe foo\" + Enter + "bar" + Enter -- bash-style line
     * continuation, backslash-newline dropped entirely -- must run as one
     * word, "foobar", not two arguments. */
    static const char continued[] = "shell_test_probe foo\\\nbar\n";
    bool ok = shell_console__is_ready() &&
              stdio__session_write_input(session, continued, strlen(continued)) == BRUCE_OK;
    char drain[256];
    size_t drain_size = 0;
    uint64_t wait_start = runtime__now();
    while (ok && s_probe_calls == 0 && runtime__now() - wait_start < 3000) {
        (void)stdio__session_read_output(session, drain, sizeof(drain), &drain_size);
        (void)runtime__delay(10);
    }
    ok = ok && s_probe_calls == 1 && strcmp(s_probe_arg, "foobar") == 0;
    if (!ok) {
        printf(
            "[selftest] shell/history-multiline: continuation exec failed (calls=%d arg=%s)\n", s_probe_calls,
            s_probe_arg
        );
    }

    /* An if/then/.../fi block spanning four physical lines -- one logical
     * command, same as the continuation above. */
    static const char block[] = "if true\nthen\nshell_test_probe block_ran\nfi\n";
    ok = ok && stdio__session_write_input(session, block, strlen(block)) == BRUCE_OK;
    wait_start = runtime__now();
    while (ok && s_probe_calls == 1 && runtime__now() - wait_start < 3000) {
        (void)stdio__session_read_output(session, drain, sizeof(drain), &drain_size);
        (void)runtime__delay(10);
    }
    ok = ok && s_probe_calls == 2 && strcmp(s_probe_arg, "block_ran") == 0;
    if (!ok) {
        printf(
            "[selftest] shell/history-multiline: if-block exec failed (calls=%d arg=%s)\n", s_probe_calls,
            s_probe_arg
        );
    }

    static const char exit_line[] = "exit\n";
    ok = ok && stdio__session_write_input(session, exit_line, strlen(exit_line)) == BRUCE_OK;
    bruce_process_status_t status;
    uint64_t exit_wait_start = runtime__now();
    bruce_result_t wait_result;
    while ((wait_result = process__wait_status(shell_id, 0, &status)) == BRUCE_ERR_TIMEOUT &&
           runtime__now() - exit_wait_start < 3000) {
        (void)stdio__session_read_output(session, drain, sizeof(drain), &drain_size);
        (void)runtime__delay(10);
    }
    ok = ok && wait_result == BRUCE_OK && status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0;
    if (!ok) printf("[selftest] shell/history-multiline: shell didn't exit cleanly\n");
    (void)stdio__session_close(session);

    /* Read /.shell_history unconditionally (not gated on `ok` above) --
     * whether or not the commands ran, this confirms exactly what got
     * recorded: each multi-line command -- the backslash-continued one and
     * the if-block -- must land as exactly one flattened entry, never one
     * per physical line (shell_history.c's file format is strictly one
     * line per entry; see shell_app__history_flatten()). */
    char history[512] = {0};
    size_t history_size = 0;
    bruce_file_id_t history_file = BRUCE_FILE_ID_INVALID;
    bool history_ok = storage__open(SHELL_HISTORY_PATH, BRUCE_STORAGE_OPEN_READ, &history_file) == BRUCE_OK &&
                       storage__read(history_file, history, sizeof(history) - 1, &history_size) == BRUCE_OK;
    if (history_file != BRUCE_FILE_ID_INVALID) (void)storage__close(history_file);
    if (history_ok) history[history_size] = '\0';

    /* Exactly 3 entries (one '\n' each): the flattened backslash-continued
     * command, the flattened if-block, and "exit" -- never one entry per
     * physical line typed. */
    size_t newline_count = 0;
    for (size_t i = 0; i < history_size; ++i) {
        if (history[i] == '\n') newline_count++;
    }
    history_ok = history_ok && newline_count == 3 && strstr(history, "shell_test_probe foobar\n") != NULL &&
                 strstr(history, "if true; then shell_test_probe block_ran; fi\n") != NULL &&
                 strstr(history, "exit\n") != NULL;
    if (!history_ok) printf("[selftest] shell/history-multiline: history=%s\n", history);
    ok = ok && history_ok;

    (void)storage__remove(SHELL_HISTORY_PATH);
    printf("[selftest] shell/history-multiline: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_jobs_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));

    /* "cmd &" returns immediately (status 0) and records exactly one
     * running job, with $! (last_background_pid) pointing at it. */
    bool ok = shell__execute_line(&state, "shell_test_probe backgrounded &") == 0 && state.job_count == 1 &&
        state.jobs[0].number == 1 && state.jobs[0].pid != BRUCE_PROCESS_ID_INVALID &&
        state.last_background_pid == state.jobs[0].pid;

    /* Bare "wait" blocks for every tracked job and reaps it -- by the time
     * it returns, the probe has actually run. */
    ok = ok && shell__execute_line(&state, "wait") == 0 && state.job_count == 0 && s_probe_calls == 1 &&
        strcmp(s_probe_arg, "backgrounded") == 0;

    /* "wait %N" blocks for just that job and returns its real exit status
     * (37, via the probe's "nonzero" path -- see the synchronous
     * "shell_test_probe nonzero" case in selftest__run_shell_language_case()). */
    ok = ok && shell__execute_line(&state, "shell_test_probe nonzero &") == 0 && state.job_count == 1;
    char wait_line[24];
    snprintf(wait_line, sizeof(wait_line), "wait %%%d", ok ? state.jobs[0].number : 0);
    ok = ok && shell__execute_line(&state, wait_line) == 37 && state.job_count == 0;

    /* Only external commands and functions can be backgrounded: a builtin,
     * a redirected command, an arithmetic command, and a pipeline all reject
     * "&" outright with status 2 rather than silently running synchronously
     * or leaving an untracked job behind. */
    ok = ok && shell__execute_line(&state, "cd &") == 2 && state.job_count == 0;
    ok = ok &&
        shell__execute_line(&state, "shell_test_probe redirected > /apps/shell_jobs_test_redirect.txt &") == 2 &&
        state.job_count == 0;
    ok = ok && shell__execute_line(&state, "((1 + 1)) &") == 2 && state.job_count == 0;
    ok = ok && shell__execute_line(&state, "shell_test_probe a | shell_test_probe b &") == 2 &&
        state.job_count == 0;

    /* A backgrounded function runs as its own subshell process: it inherits
     * the parent's exported variables (same mechanism as the $INHERITED_SHELL
     * case in selftest__run_shell_language_case()) but its own state never
     * leaks back into the parent. */
    ok = ok && shell__execute_line(&state, "export VISIBLE=parent") == 0;
    ok = ok && shell__execute_line(&state, "leaker() { shell_test_probe $VISIBLE parent; }") == 0;
    ok = ok && shell__execute_line(&state, "leaker &") == 0 && state.job_count == 1;
    snprintf(wait_line, sizeof(wait_line), "wait %%%d", ok ? state.jobs[0].number : 0);
    ok = ok && shell__execute_line(&state, wait_line) == 0 && state.job_count == 0;
    ok = ok && shell__execute_line(&state, "unset VISIBLE") == 0;

    shell__state_free(&state);
    (void)storage__remove("/apps/shell_jobs_test_redirect.txt");
    printf("[selftest] shell/jobs: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_kill_case(void) {
    if (!selftest__shell_register_spin_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);

    /* Bare "kill %N" (no -s/-SIGSPEC) defaults to TERM, same as bash: the
     * spinning job actually stops, and "wait" reports the 128+signal exit
     * code shell_executor__status_to_exit_code() uses for a signal death. */
    bool ok = shell__execute_line(&state, "shell_test_spin &") == 0 && state.job_count == 1;
    int job_number = ok ? state.jobs[0].number : 0;
    char line[24];
    snprintf(line, sizeof(line), "kill %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 0;
    snprintf(line, sizeof(line), "wait %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 128 + BRUCE_PROCESS_SIGNAL_TERM && state.job_count == 0;

    /* "kill -s KILL <pid>" (raw PID, not "%N") reaches an untracked-by-number
     * target the same way real kill(1) can signal any PID -- not just a job
     * this shell happens to be tracking. */
    ok = ok && shell__execute_line(&state, "shell_test_spin &") == 0 && state.job_count == 1;
    bruce_process_id_t pid = ok ? state.jobs[0].pid : BRUCE_PROCESS_ID_INVALID;
    job_number = ok ? state.jobs[0].number : 0;
    snprintf(line, sizeof(line), "kill -s KILL %u", (unsigned)pid);
    ok = ok && shell__execute_line(&state, line) == 0;
    snprintf(line, sizeof(line), "wait %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 128 + BRUCE_PROCESS_SIGNAL_KILL && state.job_count == 0;

    /* A bare "-SIGSPEC" token (bash's other kill syntax) works the same as
     * "-s SIGNAL", found anywhere among the arguments. */
    ok = ok && shell__execute_line(&state, "shell_test_spin &") == 0 && state.job_count == 1;
    job_number = ok ? state.jobs[0].number : 0;
    snprintf(line, sizeof(line), "kill -INT %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 0;
    snprintf(line, sizeof(line), "wait %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 128 + BRUCE_PROCESS_SIGNAL_INT && state.job_count == 0;

    /* Error paths: an unresolvable target and an invalid signal are each
     * reported and fail (status 1) without touching the job table; no
     * targets at all is a usage error, same status. */
    ok = ok && shell__execute_line(&state, "kill %99") == 1 && state.job_count == 0;
    ok = ok && shell__execute_line(&state, "kill -s BOGUS %99") == 1;
    ok = ok && shell__execute_line(&state, "kill") == 1;

    shell__state_free(&state);
    printf("[selftest] shell/kill: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_shell_job_control_case(void) {
    if (!selftest__shell_register_probe() || !selftest__shell_register_spin_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));

    /* "fg %N" prints the job's command line, then blocks the same way an
     * ordinary foreground command does (shell_executor__wait()) -- by the
     * time it returns, the probe has actually run and the job is reaped. */
    bool ok = shell__execute_line(&state, "shell_test_probe fg_target &") == 0 && state.job_count == 1;
    int job_number = ok ? state.jobs[0].number : 0;
    char line[32];
    snprintf(line, sizeof(line), "fg %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 0 && state.job_count == 0 && s_probe_calls == 1 &&
         strcmp(s_probe_arg, "fg_target") == 0;

    /* Bare "fg" (no job spec) defaults to the most recently backgrounded
     * job -- the older one is left alone -- and relays the job's own real
     * exit status (37, via the probe's "nonzero" path), not just 0/1.
     * The older job is a long-running spin process (not a quick-exiting
     * probe): a job that has *already exited* but was never waited on can
     * have its exit status silently evicted from the core process layer's
     * completion table by an unrelated process's exit publishing into the
     * same slot (process.c's process__publish_completion_locked() reuses
     * any slot with waiter_pins == 0, i.e. nothing currently blocked on
     * it) -- a pre-existing core race, not something this job-control
     * layer can paper over. Keeping "first" genuinely still-running until
     * the bare "wait" below explicitly kills and reaps it sidesteps that
     * race entirely. */
    ok = ok && shell__execute_line(&state, "shell_test_spin &") == 0 &&
         shell__execute_line(&state, "shell_test_probe nonzero &") == 0 && state.job_count == 2;
    bruce_process_id_t first_pid = ok ? state.jobs[0].pid : BRUCE_PROCESS_ID_INVALID;
    ok = ok && shell__execute_line(&state, "fg") == 37 && state.job_count == 1;
    ok = ok && process__signal(first_pid, BRUCE_PROCESS_SIGNAL_TERM) == BRUCE_OK;
    /* Bare "wait" (like bash's) always reports status 0 regardless of the
     * individual jobs' own exit statuses -- only "wait %N" relays a job's
     * real status (see the "kill %N"/"wait %N" pair further down, and
     * selftest__run_shell_jobs_case()'s own bare-"wait" check). */
    ok = ok && shell__execute_line(&state, "wait") == 0 && state.job_count == 0;

    /* Error paths: nothing to pick ("no current job") and an unresolvable
     * explicit target ("no such job") each fail without side effects. */
    ok = ok && shell__execute_line(&state, "fg") == 1 && shell__execute_line(&state, "fg %99") == 1;

    /* "bg %N" only does anything to an actually-paused job -- nothing in
     * this shell itself ever pauses one (see shell_jobs__bg()'s own doc
     * comment), so the test pauses it directly via the same core_sdk
     * process__pause() a script's "process pause <pid>" would eventually
     * reach. Resuming turns it back into an ordinary running background job,
     * which "kill"/"wait" then reap normally. */
    ok = ok && shell__execute_line(&state, "shell_test_spin &") == 0 && state.job_count == 1;
    bruce_process_id_t spin_pid = ok ? state.jobs[0].pid : BRUCE_PROCESS_ID_INVALID;
    job_number = ok ? state.jobs[0].number : 0;
    ok = ok && process__pause(spin_pid) == BRUCE_OK;
    bruce_process_snapshot_t snapshot;
    ok = ok && process__snapshot(spin_pid, &snapshot) == BRUCE_OK && snapshot.state == BRUCE_PROCESS_PAUSED;
    snprintf(line, sizeof(line), "bg %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 0 && state.job_count == 1;
    ok = ok && process__snapshot(spin_pid, &snapshot) == BRUCE_OK && snapshot.state != BRUCE_PROCESS_PAUSED;

    /* "bg" on a job that's already running (not paused) is an error, same as
     * bash's "job already in background". */
    ok = ok && shell__execute_line(&state, line) == 1;

    snprintf(line, sizeof(line), "kill %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 0;
    snprintf(line, sizeof(line), "wait %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 128 + BRUCE_PROCESS_SIGNAL_TERM && state.job_count == 0;

    ok = ok && shell__execute_line(&state, "bg") == 1 && shell__execute_line(&state, "bg %99") == 1;

    /* "disown %N" drops a job from the table without touching the process
     * itself -- it keeps running (or, as here, simply finishes on its own)
     * with no "[N]+ Done" notification, since shell_jobs__poll() no longer
     * knows about it once it's out of the table. */
    ok = ok && shell__execute_line(&state, "shell_test_probe disowned &") == 0 && state.job_count == 1;
    job_number = ok ? state.jobs[0].number : 0;
    snprintf(line, sizeof(line), "disown %%%d", job_number);
    ok = ok && shell__execute_line(&state, line) == 0 && state.job_count == 0;
    ok = ok && shell__execute_line(&state, "disown") == 1 && shell__execute_line(&state, "disown %99") == 1;

    shell__state_free(&state);
    printf("[selftest] shell/job-control: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Runs `command` (a "shell -c ..." argument string) as a background child
 * with no stdin needed and checks it exits with `expected_exit` -- proof
 * that shell_builtins__fire_exit_trap() is actually wired into
 * shell_app_main()'s "-c" exit point, since shell__execute_line() (used by
 * every in-process check below) never calls it on its own. */
static bool selftest__shell_trap_exit_probe(const char *command, int expected_exit) {
    int launched = app_runner__run("shell", command, BRUCE_LAUNCH_BACKGROUND);
    if (launched <= 0) return false;
    bruce_process_status_t status;
    return process__wait_status((bruce_process_id_t)launched, 2000, &status) == BRUCE_OK &&
           status.reason == BRUCE_PROCESS_EXITED && status.exit_code == expected_exit;
}

/* Covers the `trap` builtin (shell_builtins.c) and its two firing functions:
 * in-process checks drive a shell_state_t directly (calling
 * shell_builtins__fire_exit_trap() explicitly, the same way shell_app.c's
 * own exit points do -- shell__execute_line() alone never reaches it), then
 * a handful of real spawned processes confirm those exit points and the two
 * live-signal poll sites (the idle prompt and
 * shell_compound__loop_should_stop()) actually call it, matching bash's own
 * EXIT/INT/TERM trap semantics -- including an EXIT trap that calls `exit`
 * itself overriding the status the run was already ending with. Known,
 * documented scope limitation (see shell_builtins__fire_signal_trap()'s own
 * doc comment and the README): a foreground external command running
 * synchronously outside a loop isn't covered by either poll site, so an
 * INT/TERM trap doesn't preempt it -- not exercised here since there's
 * nothing to assert against. */
bool selftest__run_shell_trap_case(void) {
    if (!selftest__shell_register_probe()) return false;
    shell_state_t state;
    shell__state_init(&state);
    memset(s_probe_arg, 0, sizeof(s_probe_arg));
    bool ok =
        /* Bare "trap"/"trap -l" with nothing set: no crash, exit 0. */
        shell__execute_line(&state, "trap") == 0 && shell__execute_line(&state, "trap -l") == 0 &&
        /* Rejects an unknown or untrappable (KILL, by name or number)
         * signal spec instead of silently accepting it. */
        shell__execute_line(&state, "trap 'x' NOSUCHSIGNAL") == 2 &&
        shell__execute_line(&state, "trap 'x' KILL") == 2 && shell__execute_line(&state, "trap 'x' 9") == 2 &&
        shell__execute_line(&state, "trap 'shell_test_probe exit-trap' EXIT") == 0;
    shell_builtins__fire_exit_trap(&state);
    ok = ok && strcmp(s_probe_arg, "exit-trap") == 0;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));
    /* trap_exit_fired guards against a second run for this same state. */
    shell_builtins__fire_exit_trap(&state);
    ok = ok && s_probe_arg[0] == '\0';
    /* "trap - EXIT" resets the slot back to the default -- a later fire (on
     * a state that hasn't already fired) then does nothing at all. */
    state.trap_exit_fired = false;
    ok = ok && shell__execute_line(&state, "trap - EXIT") == 0;
    shell_builtins__fire_exit_trap(&state);
    ok = ok && s_probe_arg[0] == '\0';
    shell__state_free(&state);

    /* An EXIT trap that doesn't itself call `exit` never overrides the
     * status this run was already ending with. */
    shell_state_t status_state;
    shell__state_init(&status_state);
    ok = ok && shell__execute_line(&status_state, "trap 'shell_test_probe ran' EXIT") == 0 &&
         shell__execute_line(&status_state, "exit 7") == 7;
    shell_builtins__fire_exit_trap(&status_state);
    ok = ok && status_state.exit_status == 7 && strcmp(s_probe_arg, "ran") == 0;
    shell__state_free(&status_state);

    /* ...but an EXIT trap that *does* call `exit` itself overrides that
     * status -- matching bash. */
    shell_state_t override_state;
    shell__state_init(&override_state);
    ok = ok && shell__execute_line(&override_state, "trap 'exit 5' EXIT") == 0 &&
         shell__execute_line(&override_state, "exit 3") == 3;
    shell_builtins__fire_exit_trap(&override_state);
    ok = ok && override_state.exit_status == 5;
    shell__state_free(&override_state);

    /* `trap '' SIGSPEC` ("ignore") is distinct from never setting a trap at
     * all: it's still "set" (non-NULL, so it shows up in `trap`'s listing
     * and clearing it later with "trap - SIGSPEC" is meaningful) but firing
     * it runs nothing. */
    shell_state_t ignore_state;
    shell__state_init(&ignore_state);
    memset(s_probe_arg, 0, sizeof(s_probe_arg));
    ok = ok && shell__execute_line(&ignore_state, "trap '' EXIT") == 0;
    shell_builtins__fire_exit_trap(&ignore_state);
    ok = ok && s_probe_arg[0] == '\0' && ignore_state.trap_exit != NULL && ignore_state.trap_exit[0] == '\0';
    shell__state_free(&ignore_state);

    /* -- Black-box: a real spawned "-c" process actually reaches
     * shell_app_main()'s own exit point. */
    ok = ok && selftest__shell_trap_exit_probe("-c \"trap 'shell_test_probe fired' EXIT; true\"", 0) &&
         strcmp(s_probe_arg, "fired") == 0 &&
         selftest__shell_trap_exit_probe("-c \"trap 'exit 5' EXIT; exit 3\"", 5);

    /* -- "trap" (bare) listing format, bash's own "trap -- 'ACTION' SIGSPEC"
     * per line -- needs a real spawned process to capture stdout from. */
    bruce_stdio_session_t list_session = BRUCE_STDIO_SESSION_INVALID;
    ok = ok && stdio__session_create(&list_session) == BRUCE_OK &&
         stdio__session_route_children(list_session) == BRUCE_OK;
    int list_launched =
        ok ? app_runner__run("shell", "-c \"trap 'echo hi' INT; trap\"", BRUCE_LAUNCH_BACKGROUND) : 0;
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    bruce_process_status_t list_status;
    ok = ok && list_launched > 0 &&
         process__wait_status((bruce_process_id_t)list_launched, 2000, &list_status) == BRUCE_OK &&
         list_status.reason == BRUCE_PROCESS_EXITED && list_status.exit_code == 0;
    char list_output[128] = {0};
    size_t list_output_size = 0;
    (void)stdio__session_read_output(list_session, list_output, sizeof(list_output) - 1, &list_output_size);
    (void)stdio__session_close(list_session);
    ok = ok && strstr(list_output, "trap -- 'echo hi' INT") != NULL;

    /* -- A live INT signal at the idle prompt: shell_app.c's own handler
     * runs the trapped action instead of the default "^C, fresh prompt" --
     * the shell keeps running afterward (nothing here calls exit), and a
     * later line still runs normally. */
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    ok = ok && stdio__session_create(&session) == BRUCE_OK && stdio__session_route_children(session) == BRUCE_OK;
    shell_console__reset_ready();
    int launched = ok ? app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND) : 0;
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    ok = ok && launched > 0;
    if (ok) {
        bruce_process_id_t shell_id = (bruce_process_id_t)launched;
        uint64_t started = runtime__now();
        while (!shell_console__is_ready() && runtime__now() - started < 1000) (void)runtime__delay(5);
        /* The second line is itself a shell_test_probe call (not `echo`) so
         * this can be confirmed via s_probe_calls/s_probe_arg -- the same
         * way selftest__run_shell_history_multiline_case confirms a
         * multi-line block actually ran -- instead of scraping raw session
         * output for a marker. That raw output is mostly per-keystroke ANSI
         * redraw noise (the console redraws the whole prompt+line on every
         * character), easily hundreds of bytes for even a short line, so a
         * fixed-size capture buffer can fill with redraw noise before the
         * actual command output ever appears in it. */
        static const char set_trap[] =
            "trap 'shell_test_probe int-trapped' INT\nshell_test_probe trap-set-confirmed\n";
        s_probe_calls = 0;
        memset(s_probe_arg, 0, sizeof(s_probe_arg));
        ok = shell_console__is_ready() &&
             stdio__session_write_input(session, set_trap, strlen(set_trap)) == BRUCE_OK;

        /* Keep draining the session's output while the shell echoes and runs
         * this line: stdio__session_push_output_byte() blocks (busy-waits)
         * once the fixed-size output buffer fills, so a child that's still
         * echoing keystrokes would otherwise stall forever mid-line if
         * nobody ever reads its output (see the history/multiline case's
         * own comment on this same hazard). The drained bytes themselves are
         * discarded -- s_probe_calls is the actual signal being waited on. */
        char drain[256];
        size_t drain_size = 0;
        uint64_t wait_start = runtime__now();
        while (ok && s_probe_calls == 0 && runtime__now() - wait_start < 3000) {
            (void)stdio__session_read_output(session, drain, sizeof(drain), &drain_size);
            (void)runtime__delay(10);
        }
        ok = ok && s_probe_calls == 1 && strcmp(s_probe_arg, "trap-set-confirmed") == 0;
        memset(s_probe_arg, 0, sizeof(s_probe_arg));

        ok = ok && process__signal(shell_id, BRUCE_PROCESS_SIGNAL_INT) == BRUCE_OK;
        if (ok) (void)runtime__delay(100);
        bruce_process_status_t status;
        bruce_result_t wait_result = process__wait_status(shell_id, 0, &status);
        /* Still running -- the trap ran instead of ending the shell. */
        ok = ok && wait_result == BRUCE_ERR_TIMEOUT && strcmp(s_probe_arg, "int-trapped") == 0;
        static const char exit_line[] = "exit\n";
        ok = ok && stdio__session_write_input(session, exit_line, strlen(exit_line)) == BRUCE_OK;
        ok = ok && process__wait_status(shell_id, 2000, &status) == BRUCE_OK &&
             status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0;
    }
    (void)stdio__session_close(session);

    /* -- Same live signal, but consulted from
     * shell_compound__loop_should_stop() instead of the idle prompt: an INT
     * trap that calls `exit` is what finally ends a "while true" loop that
     * would otherwise never stop on its own. */
    int loop_launched = app_runner__run(
        "shell", "-c \"trap 'exit 42' INT; while true; do :; done\"", BRUCE_LAUNCH_BACKGROUND
    );
    ok = ok && loop_launched > 0;
    if (ok) {
        (void)runtime__delay(100); /* let the loop actually start spinning */
        bruce_process_id_t loop_id = (bruce_process_id_t)loop_launched;
        bruce_process_status_t loop_status;
        ok = ok && process__signal(loop_id, BRUCE_PROCESS_SIGNAL_INT) == BRUCE_OK &&
             process__wait_status(loop_id, 2000, &loop_status) == BRUCE_OK &&
             loop_status.reason == BRUCE_PROCESS_EXITED && loop_status.exit_code == 42;
    }

    printf("[selftest] shell/trap: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* Runs COMMAND (a `printf ... > PATH` line, PATH truncated/created fresh by
 * the redirect itself), then reads PATH back and compares its whole content
 * against EXPECTED -- the same "redirect a builtin's output to a real file,
 * then read the file back" pattern selftest__run_shell_builtin_redirect_case()
 * establishes, reused here since it needs no session/child-process machinery
 * at all for output this deterministic (unlike an interactive read()'s live
 * echo, a builtin's redirected output has nothing timing-sensitive about
 * it). Returns false (and prints a diagnostic) if COMMAND's own exit status
 * isn't 0, the file's content doesn't match EXPECTED byte-for-byte, or the
 * two APPEND commands (each optional, NULL to skip) don't also succeed --
 * used to build up multi-line expected content across more than one printf
 * call against the same file. */
static bool selftest__shell_printf_probe(
    shell_state_t *state, const char *path, const char *command, const char *append1, const char *append2,
    const char *expected
) {
    (void)storage__remove(path);
    int status = shell__execute_line(state, command);
    if (status == 0 && append1 != NULL) status = shell__execute_line(state, append1);
    if (status == 0 && append2 != NULL) status = shell__execute_line(state, append2);
    char result[128] = {0};
    size_t result_size = 0;
    bruce_result_t read_result = BRUCE_ERR_NOT_FOUND;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        read_result = storage__read(file, result, sizeof(result) - 1, &result_size);
        (void)storage__close(file);
    }
    size_t expected_length = strlen(expected);
    bool ok = status == 0 && read_result == BRUCE_OK && result_size == expected_length &&
              memcmp(result, expected, expected_length) == 0;
    if (!ok) {
        printf(
            "[selftest] shell/printf: %s failed (status=%d read=%d result=%.*s)\n", command, status,
            (int)read_result, (int)result_size, result
        );
    }
    (void)storage__remove(path);
    return ok;
}

bool selftest__run_shell_printf_case(void) {
    shell_state_t state;
    shell__state_init(&state);

    /* Every expected[] string below spells a printf-emitted newline as
     * "\r\n", not "\n" -- same ONLCR session-output convention noted on
     * selftest__run_shell_output_redirect_case()'s and
     * selftest__run_shell_builtin_redirect_case()'s own expected[] arrays:
     * the capture session redirected output goes through applies it to
     * every '\n' a builtin writes, printf included. */
    bool ok =
        selftest__shell_printf_probe(
            &state, "/apps/shell_printf_basic.txt", "printf '%s-%d\\n' foo 42 > /apps/shell_printf_basic.txt", NULL,
            NULL, "foo-42\r\n"
        ) &&
        /* `\t`/`\n` reach printf's own FORMAT argument as literal backslash
         * pairs -- single-quoted, so this shell's own parser doesn't decode
         * them first -- for printf's own escape decoding to turn into a real
         * tab/newline; `%%` is a literal '%', consuming no argument. */
        selftest__shell_printf_probe(
            &state, "/apps/shell_printf_escape.txt",
            "printf 'tab\\there %%\\n' > /apps/shell_printf_escape.txt", NULL, NULL, "tab\there %\r\n"
        ) &&
        /* Width/flags (right-justify, left-justify, zero-pad) pass straight
         * through to the underlying vsnprintf via the reconstructed
         * "%5ld"/"%-5ld"/"%05ld" sub-format -- see shell_builtins__printf(). */
        selftest__shell_printf_probe(
            &state, "/apps/shell_printf_width.txt", "printf '[%5d]\\n' 3 > /apps/shell_printf_width.txt",
            "printf '[%-5d]\\n' 3 >> /apps/shell_printf_width.txt",
            "printf '[%05d]\\n' 7 >> /apps/shell_printf_width.txt", "[    3]\r\n[3    ]\r\n[00007]\r\n"
        );
    ok = ok && selftest__shell_printf_probe(
                    &state, "/apps/shell_printf_radix.txt",
                    "printf '%x %X %o\\n' 255 255 8 > /apps/shell_printf_radix.txt", NULL, NULL, "ff FF 10\r\n"
                );
    /* %c prints just the ARG's first character -- and, unlike bash embedding
     * a stray NUL byte for one, nothing at all when the ARG is empty. */
    ok = ok && selftest__shell_printf_probe(
                    &state, "/apps/shell_printf_char.txt", "printf '<%c>\\n' hello > /apps/shell_printf_char.txt",
                    "printf '<%c>\\n' '' >> /apps/shell_printf_char.txt", NULL, "<h>\r\n<>\r\n"
                );
    /* %b additionally backslash-decodes its own ARG (here, single-quoted so
     * this shell's parser leaves the "\n" untouched for printf itself to
     * decode) on top of %s's usual behavior -- FORMAT's own trailing "\n" is
     * a second, separately-decoded newline. */
    ok = ok && selftest__shell_printf_probe(
                    &state, "/apps/shell_printf_b.txt", "printf '%b\\n' 'literal\\nescape' > /apps/shell_printf_b.txt",
                    NULL, NULL, "literal\r\nescape\r\n"
                );
    /* FORMAT recycles against leftover ARGUMENTs when it has at least one
     * argument-consuming conversion (three ARGs, one %s -> three passes) --
     * but never recycles at all when it has none, no matter how many extra
     * ARGUMENTs were given. */
    ok = ok && selftest__shell_printf_probe(
                    &state, "/apps/shell_printf_recycle.txt", "printf '[%s]' a b c > /apps/shell_printf_recycle.txt",
                    "printf '\\n' >> /apps/shell_printf_recycle.txt", NULL, "[a][b][c]\r\n"
                );
    ok = ok && selftest__shell_printf_probe(
                    &state, "/apps/shell_printf_no_recycle.txt",
                    "printf 'plain\\n' ignored extra > /apps/shell_printf_no_recycle.txt", NULL, NULL, "plain\r\n"
                );
    /* A conversion with no ARGUMENT left (here, none given at all) uses ""
     * for %s and 0 for a numeric conversion instead of erroring. */
    ok = ok && selftest__shell_printf_probe(
                    &state, "/apps/shell_printf_missing.txt",
                    "printf '[%s][%d]\\n' > /apps/shell_printf_missing.txt", NULL, NULL, "[][0]\r\n"
                );

    /* No FORMAT at all: usage error, status 2 -- same convention `trap`'s
     * own usage error uses. An unrecognized conversion character: status 1,
     * matching bash's real printf (distinct from the usage-error case). */
    ok = ok && shell__execute_line(&state, "printf") == 2 && shell__execute_line(&state, "printf '%q\\n'") == 1;
    shell__state_free(&state);

    printf("[selftest] shell/printf: %s\n", ok ? "OK" : "failed");
    return ok;
}
