#include "shell_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "modules/shell/shell_app.h"

static volatile int s_probe_calls;
static char s_probe_arg[64];

static int selftest__shell_probe(int argc, char **argv) {
    s_probe_calls++;
    snprintf(s_probe_arg, sizeof(s_probe_arg), "%s", argc > 1 ? argv[1] : "");
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
    shell_state_t state;
    shell__state_init(&state);
    s_probe_calls = 0;
    memset(s_probe_arg, 0, sizeof(s_probe_arg));

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
        shell__execute_line(&state, "export EXPORTED=yes; unset EXPORTED; set") == 0 &&
        shell__execute_line(&state, "shell_test_probe nonzero") == 37 &&
        shell__execute_line(&state, "echo broken | echo nope") == 2 &&
        shell__execute_line(&state, "echo > file") == 2 &&
        shell__execute_line(&state, "echo 'unterminated") == 2;
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
    if (bruce_stdio_session_create(&session) != BRUCE_OK ||
        bruce_stdio_session_route_children(session) != BRUCE_OK) {
        return false;
    }
    int launched = app_runner__run("shell", "-c \"shell_test_probe routed\"", true);
    (void)bruce_stdio_session_route_children(BRUCE_STDIO_SESSION_INVALID);
    bruce_process_status_t status;
    bool completed =
        launched > 0 && process__wait_status((bruce_process_id_t)launched, 2000, &status) == BRUCE_OK;
    char output[128] = {0};
    size_t size = 0;
    bruce_result_t read = bruce_stdio_session_read_output(session, output, sizeof(output) - 1, &size);
    (void)bruce_stdio_session_close(session);
    bool ok = completed && status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0 &&
              read == BRUCE_OK && strstr(output, "shell-grandchild-routed") != NULL;
    printf("[selftest] shell/stdio-inheritance: %s\n", ok ? "OK" : "failed");
    return ok;
}
