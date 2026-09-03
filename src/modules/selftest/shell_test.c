#include "shell_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"
#include "modules/shell/shell_app.h"
#include "modules/shell/shell_builtins.h"
#include "modules/shell/shell_console.h"
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
    /* Malformed constructs are reported, not silently misparsed. */
    ok = selftest__shell_loops_step(ok, &state, "for x in a b", 2, NULL);
    ok = selftest__shell_loops_step(ok, &state, "while true; do echo hi", 2, NULL);
    ok = selftest__shell_loops_step(ok, &state, "(( 1 +", 2, NULL);

    shell__state_free(&state);
    printf("[selftest] shell/loops: %s\n", ok ? "OK" : "failed");
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

/* Exercises "cmd < file" -- shell_executor__external_input_redirected() in
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
         * shell_executor__builtin_input_redirected() fails to open it via
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
