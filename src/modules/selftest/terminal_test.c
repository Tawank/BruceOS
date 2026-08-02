#include "terminal_test.h"

#include <stdio.h>
#include <string.h>

#include "core/storage/storage.h" // IWYU pragma: keep
#include "core_sdk/app_runner.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "fake_elf.h"
#include "modules/loaders/elf/elf_loader_app.h"
#include "modules/shell/shell_history.h"
#include "modules/shell/shell_line_editor.h"
#include "modules/utils/serial_commands/serial_commands_app.h"
#include "modules/utils/terminal/terminal_ansi.h"

/* ------------------------------------------------------------------------ */
/* Terminal parser: named built-in dispatch                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile int argc;
    char argv_buf[4][48];
} terminal_test_echo_t;

static terminal_test_echo_t s_echo;

static int selftest__terminal_test_echo_entry(int argc, char **argv) {
    s_echo.argc = argc;
    for (int i = 0; i < argc && i < 4; ++i) {
        strncpy(s_echo.argv_buf[i], argv[i], sizeof(s_echo.argv_buf[i]) - 1);
        s_echo.argv_buf[i][sizeof(s_echo.argv_buf[i]) - 1] = '\0';
    }
    return 0;
}

bool selftest__run_terminal_named_case(void) {
    memset(&s_echo, 0, sizeof(s_echo));

    bruce_result_t registered =
        app_runner__register("terminal_test_echo", selftest__terminal_test_echo_entry, 0);
    if (registered != BRUCE_OK && registered != BRUCE_ERR_ALREADY_EXISTS) {
        printf("[selftest] terminal/named: failed to register echo app (%d)\n", registered);
        return false;
    }

    int result = serial_commands__run_line("terminal_test_echo hello \"world now\"", false);
    if (result <= 0) {
        printf("[selftest] terminal/named: run_line returned %d\n", result);
        return false;
    }

    bruce_result_t wait_result = process__wait((bruce_process_id_t)result, 2000);
    if (wait_result != BRUCE_OK && wait_result != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] terminal/named: wait failed (%d)\n", wait_result);
        return false;
    }

    if (s_echo.argc != 3 || strcmp(s_echo.argv_buf[0], "terminal_test_echo") != 0 ||
        strcmp(s_echo.argv_buf[1], "hello") != 0 || strcmp(s_echo.argv_buf[2], "world now") != 0) {
        printf(
            "[selftest] terminal/named: argc=%d argv=[%s|%s|%s]\n",
            s_echo.argc,
            s_echo.argv_buf[0],
            s_echo.argv_buf[1],
            s_echo.argv_buf[2]
        );
        return false;
    }

    printf("[selftest] terminal/named: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Terminal parser: path dispatch (ELF loader)                               */
/* ------------------------------------------------------------------------ */

bool selftest__run_terminal_path_case(void) {
    const char *path = "/apps/terminal_test_app.elf";
    storage__remove(path);

    if (!selftest__write_fake_elf(path, "Terminal Test App", NULL, 0)) {
        printf("[selftest] terminal/path: could not create fake ELF\n");
        return false;
    }

    size_t calls_before = elf_loader__debug_call_count();
    int result = serial_commands__run_line(path, false);
    size_t calls_after = elf_loader__debug_call_count();

    if (result > 0) { (void)process__wait((bruce_process_id_t)result, 2000); }
    storage__remove(path);

    if (result <= 0 || calls_after != calls_before + 1) {
        printf(
            "[selftest] terminal/path: ELF loader not dispatched (%d, calls %zu -> %zu)\n",
            result,
            calls_before,
            calls_after
        );
        return false;
    }

    printf("[selftest] terminal/path: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Terminal parser: invalid input                                            */
/* ------------------------------------------------------------------------ */

bool selftest__run_terminal_invalid_case(void) {
    if (serial_commands__run_line("", false) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] terminal/invalid: empty line did not return BRUCE_ERR_INVALID_ARGUMENT\n");
        return false;
    }
    if (serial_commands__run_line("   ", false) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf(
            "[selftest] terminal/invalid: whitespace-only line did not return BRUCE_ERR_INVALID_ARGUMENT\n"
        );
        return false;
    }
    if (serial_commands__run_line("selftest_terminal_unknown_command", false) != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] terminal/invalid: unknown command did not return BRUCE_ERR_NOT_FOUND\n");
        return false;
    }

    printf("[selftest] terminal/invalid: OK\n");
    return true;
}

static int selftest__terminal_stdio_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char line[32];
    if (stdio__read_line(line, sizeof(line), false) >= 0) { stdio__printf("received:%s\n", line); }
    return 0;
}

static volatile bool s_stdio_cancel_started;
static volatile bruce_result_t s_stdio_cancel_result;

static int selftest__terminal_stdio_cancel_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char byte;
    size_t size = 0;
    s_stdio_cancel_started = true;
    s_stdio_cancel_result = stdio__read(&byte, 1, UINT32_MAX, &size);
    return 0;
}

bool selftest__run_terminal_stdio_case(void) {
    bruce_result_t registered =
        app_runner__register("terminal_test_stdio", selftest__terminal_stdio_entry, 0);
    if (registered != BRUCE_OK && registered != BRUCE_ERR_ALREADY_EXISTS) return false;

    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        return false;
    }
    int result = app_runner__run("terminal_test_stdio", NULL, BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (result <= 0 || stdio__session_write_input(session, "hello\n", 6) != BRUCE_OK) {
        (void)stdio__session_close(session);
        return false;
    }
    bruce_process_status_t status;
    bruce_result_t waited = process__wait_status((bruce_process_id_t)result, 2000, &status);
    char output[128] = {0};
    size_t output_size = 0;
    bruce_result_t read_result =
        stdio__session_read_output(session, output, sizeof(output) - 1, &output_size);
    (void)stdio__session_close(session);
    bool ok = waited == BRUCE_OK && status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0 &&
              read_result == BRUCE_OK && strstr(output, "hello") != NULL &&
              strstr(output, "received:hello") != NULL;

    session = BRUCE_STDIO_SESSION_INVALID;
    if (ok && stdio__session_create(&session) == BRUCE_OK &&
        stdio__session_route_children(session) == BRUCE_OK) {
        result = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
        (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
        static const char shell_input[] = "echo interactive-ok\rexit\r";
        if (result <= 0 ||
            stdio__session_write_input(session, shell_input, sizeof(shell_input) - 1) != BRUCE_OK) {
            ok = false;
        } else {
            waited = process__wait_status((bruce_process_id_t)result, 2000, &status);
            memset(output, 0, sizeof(output));
            output_size = 0;
            read_result = stdio__session_read_output(session, output, sizeof(output) - 1, &output_size);
            ok = waited == BRUCE_OK && status.reason == BRUCE_PROCESS_EXITED &&
                 status.exit_code == 0 && read_result == BRUCE_OK &&
                 strstr(output, "interactive-ok") != NULL;
        }
        (void)stdio__session_close(session);
    } else {
        ok = false;
        if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
    }
    printf("[selftest] terminal/stdio: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_terminal_stdio_cancel_case(void) {
    bruce_result_t registered =
        app_runner__register("terminal_test_stdio_cancel", selftest__terminal_stdio_cancel_entry, 0);
    if (registered != BRUCE_OK && registered != BRUCE_ERR_ALREADY_EXISTS) return false;

    s_stdio_cancel_started = false;
    s_stdio_cancel_result = BRUCE_OK;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK || stdio__session_route_children(session) != BRUCE_OK) {
        if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
        return false;
    }

    int launched = app_runner__run("terminal_test_stdio_cancel", NULL, BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0) {
        (void)stdio__session_close(session);
        return false;
    }

    uint64_t started = runtime__now();
    while (!s_stdio_cancel_started && runtime__now() - started < 250) (void)runtime__delay(5);

    bruce_process_id_t process_id = (bruce_process_id_t)launched;
    bruce_process_status_t status;
    bruce_result_t signal_result =
        s_stdio_cancel_started ? process__signal(process_id, BRUCE_PROCESS_SIGNAL_TERM) : BRUCE_ERR_TIMEOUT;
    bruce_result_t wait_result = process__wait_status(process_id, 1000, &status);
    if (wait_result != BRUCE_OK) {
        (void)process__kill(process_id);
        (void)process__wait_status(process_id, 1000, &status);
    }
    (void)stdio__session_close(session);

    bool ok = signal_result == BRUCE_OK && wait_result == BRUCE_OK &&
              s_stdio_cancel_result == BRUCE_ERR_CANCELLED && status.reason == BRUCE_PROCESS_TERMINATED &&
              status.exit_code == 0 && status.signal == BRUCE_PROCESS_SIGNAL_TERM;
    printf("[selftest] terminal/stdio-cancel: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_terminal_editing_case(void) {
    char line[32];
    shell_line_editor_t editor;
    shell_line_editor__init(&editor, line, sizeof(line));
    bool editor_ok = shell_line_editor__insert(&editor, 'a') &&
                     shell_line_editor__insert(&editor, 'c') &&
                     shell_line_editor__left(&editor) &&
                     shell_line_editor__insert(&editor, 'b') &&
                     strcmp(line, "abc") == 0 &&
                     shell_line_editor__backspace(&editor) &&
                     shell_line_editor__delete(&editor) &&
                     strcmp(line, "a") == 0;

    char transcript[32] = {0};
    uint8_t colors[32] = {0};
    size_t transcript_size = 0;
    terminal_ansi_parser_t ansi;
    terminal_ansi__init(&ansi);
    static const char ansi_part_one[] = "plain\033[3";
    static const char ansi_part_two[] = "1mred\033[0m";
    terminal_ansi__consume(
        &ansi, ansi_part_one, sizeof(ansi_part_one) - 1, transcript, colors, &transcript_size, sizeof(transcript)
    );
    terminal_ansi__consume(
        &ansi, ansi_part_two, sizeof(ansi_part_two) - 1, transcript, colors, &transcript_size, sizeof(transcript)
    );
    bool ansi_ok = strcmp(transcript, "plainred") == 0 &&
                   colors[0] == TERMINAL_ANSI_DEFAULT_COLOR && colors[5] == 1 &&
                   ansi.color == TERMINAL_ANSI_DEFAULT_COLOR;

    char cursor_text[16] = {0};
    uint8_t cursor_colors[16] = {0};
    size_t cursor_size = 0;
    terminal_ansi_parser_t cursor_ansi;
    terminal_ansi__init(&cursor_ansi);
    static const char cursor_input[] = "abc\r\nx\033[D!\r\033[2Kbruce$ x";
    terminal_ansi__consume(
        &cursor_ansi, cursor_input, sizeof(cursor_input) - 1, cursor_text, cursor_colors,
        &cursor_size, sizeof(cursor_text)
    );
    ansi_ok = ansi_ok && strcmp(cursor_text, "abc\nbruce$ x") == 0 && cursor_ansi.cursor == cursor_size;

    static const char history_path[] = "/terminal_history_test";
    (void)storage__remove(history_path);
    bool history_ok = shell_history__append(history_path, "first") == BRUCE_OK &&
                      shell_history__append(history_path, "second") == BRUCE_OK &&
                      shell_history__append(history_path, "third") == BRUCE_OK;
    uint64_t third = 0;
    uint64_t second = 0;
    if (history_ok) {
        history_ok = shell_history__previous(history_path, UINT64_MAX, line, sizeof(line), &third) == BRUCE_OK &&
                     strcmp(line, "third") == 0 &&
                     shell_history__previous(history_path, third, line, sizeof(line), &second) == BRUCE_OK &&
                     strcmp(line, "second") == 0 &&
                     shell_history__next(history_path, second, line, sizeof(line), &third) == BRUCE_OK &&
                     strcmp(line, "third") == 0;
    }
    (void)storage__remove(history_path);

    bool ok = editor_ok && ansi_ok && history_ok;
    printf("[selftest] terminal/editing: %s\n", ok ? "OK" : "failed");
    return ok;
}
