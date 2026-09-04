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

/* Copies a grid row's glyphs (blank cells as spaces) into a NUL-terminated
 * string with trailing spaces trimmed, so it can be compared with strcmp()
 * the same way the old flat-transcript test data did. */
static void
selftest__terminal_row_text(const terminal_grid_t *grid, uint16_t row, char *out, size_t out_size) {
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

    bruce_result_t registered = app_runner__register(
        "terminal_test_echo", "Terminal argument probe", "Test", selftest__terminal_test_echo_entry, 0
    );
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
    /* serial_commands__run_line() only spawns the "elf" loader command
     * asynchronously (see app_runner__run_path_with_environment()) --
     * elf_loader__open()'s rejection of this deliberately code-less fixture
     * (see selftest__run_elf_loader_case()) happens inside that spawned
     * process, so both its call-count bump and its outcome can only be
     * observed after waiting for it to exit, not immediately after this
     * call returns (just the spawned pid on success). */
    if (result > 0) {
        bruce_process_status_t status;
        result = process__wait_status((bruce_process_id_t)result, 2000, &status) == BRUCE_OK &&
                         status.reason == BRUCE_PROCESS_EXITED
                     ? status.exit_code
                     : result;
    }
    size_t calls_after = elf_loader__debug_call_count();
    storage__remove(path);

    bool dispatched = calls_after == calls_before + 1 && result == BRUCE_ERR_INVALID_ARGUMENT;
    if (!dispatched) {
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
    bruce_result_t registered = app_runner__register(
        "terminal_test_stdio", "Terminal stdio probe", "Test", selftest__terminal_stdio_entry, 0
    );
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
    char output[2048] = {0};
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
        static const char shell_command[] = "echo interactive-ok\r";
        static const char shell_exit[] = "exit\r";
        if (result <= 0 ||
            stdio__session_write_input(session, shell_command, sizeof(shell_command) - 1) != BRUCE_OK) {
            ok = false;
        } else {
            /* The shell only echoes its output once it's actually been
             * scheduled and had a chance to parse/execute the line - a fixed
             * delay before a single read races that scheduling and was
             * intermittently failing under load. Poll instead: accumulate
             * whatever's newly available each round (stdio__session_read_output()
             * is non-blocking and only returns bytes written since the last
             * call) until the expected text shows up or a generous deadline
             * passes. */
            uint64_t deadline = runtime__now() + 2000;
            output_size = 0;
            output[0] = '\0';
            for (;;) {
                size_t chunk_size = 0;
                read_result =
                    stdio__session_read_output(session, output + output_size, sizeof(output) - 1 - output_size, &chunk_size);
                if (read_result == BRUCE_OK) {
                    output_size += chunk_size;
                    output[output_size] = '\0';
                }
                if (strstr(output, "interactive-ok") != NULL || runtime__now() >= deadline) break;
                (void)runtime__delay(10);
            }
            if (strstr(output, "interactive-ok") == NULL ||
                stdio__session_write_input(session, shell_exit, sizeof(shell_exit) - 1) != BRUCE_OK) {
                ok = false;
            }
            waited = process__wait_status((bruce_process_id_t)result, 2000, &status);
            ok = ok && waited == BRUCE_OK && status.reason == BRUCE_PROCESS_EXITED &&
                 status.exit_code == 0 && strstr(output, "interactive-ok") != NULL;
        }
        (void)stdio__session_close(session);
    } else {
        ok = false;
        if (session != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(session);
    }
    if (!ok) {
        printf(
            "[selftest] terminal/stdio: waited=%d reason=%d exit=%d read=%d size=%zu output=%s\n",
            waited,
            status.reason,
            status.exit_code,
            read_result,
            output_size,
            output
        );
    }
    printf("[selftest] terminal/stdio: %s\n", ok ? "OK" : "failed");
    return ok;
}

bool selftest__run_terminal_stdio_cancel_case(void) {
    bruce_result_t registered = app_runner__register(
        "terminal_test_stdio_cancel", "Terminal cancellation probe", "Test", selftest__terminal_stdio_cancel_entry, 0
    );
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

    terminal_cell_t sgr_cells[16];
    terminal_cell_t sgr_alt_cells[16];
    terminal_grid_t sgr_grid;
    terminal_grid__init(&sgr_grid, sgr_cells, sgr_alt_cells, 16, 1);
    static const char ansi_part_one[] = "plain\033[3";
    static const char ansi_part_two[] = "1mred\033[0m";
    terminal_grid__feed(&sgr_grid, ansi_part_one, sizeof(ansi_part_one) - 1);
    terminal_grid__feed(&sgr_grid, ansi_part_two, sizeof(ansi_part_two) - 1);
    char sgr_text[17] = {0};
    selftest__terminal_row_text(&sgr_grid, 0, sgr_text, sizeof(sgr_text));
    bool ansi_ok = strcmp(sgr_text, "plainred") == 0 && sgr_cells[0].fg == TERMINAL_ANSI_COLOR_DEFAULT &&
                   sgr_cells[5].fg == 1 && sgr_grid.fg == TERMINAL_ANSI_COLOR_DEFAULT &&
                   sgr_grid.cursor_x == 8 && sgr_grid.cursor_y == 0;

    terminal_cell_t cursor_cells[32];
    terminal_cell_t cursor_alt_cells[32];
    terminal_grid_t cursor_grid;
    terminal_grid__init(&cursor_grid, cursor_cells, cursor_alt_cells, 16, 2);
    static const char cursor_input[] = "abc\r\nx\033[D!\r\033[2Kbruce$ x";
    terminal_grid__feed(&cursor_grid, cursor_input, sizeof(cursor_input) - 1);
    char cursor_row0[17] = {0};
    char cursor_row1[17] = {0};
    selftest__terminal_row_text(&cursor_grid, 0, cursor_row0, sizeof(cursor_row0));
    selftest__terminal_row_text(&cursor_grid, 1, cursor_row1, sizeof(cursor_row1));
    ansi_ok = ansi_ok && strcmp(cursor_row0, "abc") == 0 && strcmp(cursor_row1, "bruce$ x") == 0 &&
              cursor_grid.cursor_x == 8 && cursor_grid.cursor_y == 1;

    static const char history_path[] = "/terminal_history_test";
    (void)storage__remove(history_path);
    bool history_ok = shell_history__append(history_path, "first") == BRUCE_OK &&
                      shell_history__append(history_path, "second") == BRUCE_OK &&
                      shell_history__append(history_path, "third") == BRUCE_OK &&
                      shell_history__append(history_path, "second") == BRUCE_OK &&
                      shell_history__append(history_path, "second") == BRUCE_OK &&
                      shell_history__append(history_path, "fourth") == BRUCE_OK;
    uint64_t newest = 0;
    uint64_t newer_second = 0;
    uint64_t older_second = 0;
    uint64_t first = 0;
    if (history_ok) {
        history_ok =
            shell_history__previous(history_path, UINT64_MAX, line, sizeof(line), &newest) == BRUCE_OK &&
            strcmp(line, "fourth") == 0 &&
            shell_history__previous(history_path, newest, line, sizeof(line), &newer_second) == BRUCE_OK &&
            strcmp(line, "second") == 0 &&
            shell_history__previous(history_path, newer_second, line, sizeof(line), &newest) == BRUCE_OK &&
            strcmp(line, "third") == 0 &&
            shell_history__previous(history_path, newest, line, sizeof(line), &older_second) == BRUCE_OK &&
            strcmp(line, "second") == 0 &&
            shell_history__previous(history_path, older_second, line, sizeof(line), &first) == BRUCE_OK &&
            strcmp(line, "first") == 0 &&
            shell_history__next(history_path, first, line, sizeof(line), &older_second) == BRUCE_OK &&
            strcmp(line, "second") == 0 &&
            shell_history__previous(history_path, first, line, sizeof(line), &newest) == BRUCE_ERR_NOT_FOUND;
    }
    (void)storage__remove(history_path);

    bool ok = editor_ok && ansi_ok && history_ok;
    printf("[selftest] terminal/editing: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Terminal parser: VT100 charset designation (ACS) and REP                  */
/* ------------------------------------------------------------------------ */

/* Covers the escape-handling gap that let stray bytes leak into the grid and
 * dropped repeated-character runs: G0 redesignation (smacs/rmacs, `ESC(0`/
 * `ESC(B`, what htop/tmux actually send to draw line art since this
 * emulator always advertises itself as "xterm"), G1 + SO/SI, and CSI Pn b
 * (REP). See terminal_grid__acs_translate() in terminal_ansi.c. */
bool selftest__run_terminal_ansi_escapes_case(void) {
    terminal_cell_t g0_cells[16];
    terminal_cell_t g0_alt_cells[16];
    terminal_grid_t g0_grid;
    terminal_grid__init(&g0_grid, g0_cells, g0_alt_cells, 16, 1);
    /* smacs, draw a box top ("lqqk"), rmacs, then a plain letter -- the
     * designator bytes themselves ('0', 'B') must not leak into the grid as
     * literal text. */
    static const char g0_input[] = "\033(0lqqk\033(Bx";
    terminal_grid__feed(&g0_grid, g0_input, sizeof(g0_input) - 1);
    char g0_text[17] = {0};
    selftest__terminal_row_text(&g0_grid, 0, g0_text, sizeof(g0_text));
    bool g0_ok = strcmp(g0_text, "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90x") == 0; /* "┌──┐x" */

    terminal_cell_t g1_cells[16];
    terminal_cell_t g1_alt_cells[16];
    terminal_grid_t g1_grid;
    terminal_grid__init(&g1_grid, g1_cells, g1_alt_cells, 16, 1);
    /* Designate G1 as special graphics, SO to invoke it, draw a box bottom
     * ("mvj"), SI back to G0 (still plain ASCII), then a plain letter. */
    static const char g1_input[] = "\033)0\x0emvj\x0fy";
    terminal_grid__feed(&g1_grid, g1_input, sizeof(g1_input) - 1);
    char g1_text[17] = {0};
    selftest__terminal_row_text(&g1_grid, 0, g1_text, sizeof(g1_text));
    bool g1_ok = strcmp(g1_text, "\xe2\x94\x94\xe2\x94\xb4\xe2\x94\x98y") == 0; /* "└┴┘y" */

    terminal_cell_t rep_cells[16];
    terminal_cell_t rep_alt_cells[16];
    terminal_grid_t rep_grid;
    terminal_grid__init(&rep_grid, rep_cells, rep_alt_cells, 16, 1);
    /* One 'q', then CSI 3 b repeats it 3 more times, then a plain 'X' --
     * without REP support this whole run used to just vanish. */
    static const char rep_input[] = "q\033[3bX";
    terminal_grid__feed(&rep_grid, rep_input, sizeof(rep_input) - 1);
    char rep_text[17] = {0};
    selftest__terminal_row_text(&rep_grid, 0, rep_text, sizeof(rep_text));
    bool rep_ok = strcmp(rep_text, "qqqqX") == 0;

    bool ok = g0_ok && g1_ok && rep_ok;
    printf(
        "[selftest] terminal/ansi-escapes: %s (g0=%d g1=%d rep=%d)\n", ok ? "OK" : "FAIL", g0_ok, g1_ok, rep_ok
    );
    return ok;
}
