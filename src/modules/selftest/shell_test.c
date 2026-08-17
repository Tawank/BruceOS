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
    const char *pwd = environment__get("PWD");
    snprintf(s_probe_pwd, sizeof(s_probe_pwd), "%s", pwd != NULL ? pwd : "");
    if (argc > 1 && strcmp(argv[1], "nonzero") == 0) return 37;
    if (argc > 1 && strcmp(argv[1], "routed") == 0) stdio__printf("shell-grandchild-routed\n");
    return argc > 2 && strcmp(argv[1], argv[2]) == 0 ? 0 : (argc > 2 ? 1 : 0);
}

static bool selftest__shell_register_probe(void) {
    bruce_result_t result = app_runner__register("shell_test_probe", selftest__shell_probe, 0);
    return result == BRUCE_OK || result == BRUCE_ERR_ALREADY_EXISTS;
}

bool selftest__run_shell_language_case(void) {
    if (!selftest__shell_register_probe()) return false;
    if (environment__set("INHERITED_SHELL", "visible") != BRUCE_OK) return false;
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));
    memset(s_probe_pwd, 0, sizeof(s_probe_pwd));

    bool ok =
        shell__execute_line(&state, "shell_test_probe 'a b' \"a b\"") == 0 &&
        strcmp(s_probe_arg, "a b") == 0 &&
        shell__execute_line(&state, "shell_test_probe escaped\\ word 'escaped word'") == 0 &&
        shell__execute_line(&state, "VALUE=stored; shell_test_probe $VALUE stored") == 0 &&
        shell__execute_line(&state, "false; shell_test_probe $? 1") == 0 &&
        shell__execute_line(&state, "true || shell_test_probe skipped; false && shell_test_probe skipped") ==
            1 &&
        s_probe_calls == 4 && shell__execute_line(&state, "false || true && false") == 1 &&
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
