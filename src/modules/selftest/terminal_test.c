#include "terminal_test.h"

#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"
#include "fake_elf.h"
#include "modules/loaders/elf/elf_loader.h"
#include "modules/utils/terminal/terminal.h"

/* ------------------------------------------------------------------------ */
/* Terminal parser: named built-in dispatch                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile int argc;
    char argv_buf[4][48];
} terminal_test_echo_t;

static terminal_test_echo_t s_echo;

static int selftest__terminal_test_echo_entry(int argc, char **argv)
{
    s_echo.argc = argc;
    for (int i = 0; i < argc && i < 4; ++i) {
        strncpy(s_echo.argv_buf[i], argv[i], sizeof(s_echo.argv_buf[i]) - 1);
        s_echo.argv_buf[i][sizeof(s_echo.argv_buf[i]) - 1] = '\0';
    }
    return 0;
}

bool selftest__run_terminal_named_case(void)
{
    memset(&s_echo, 0, sizeof(s_echo));

    bruce_result_t registered = app_runner__register("terminal_test_echo", selftest__terminal_test_echo_entry);
    if (registered != BRUCE_OK && registered != BRUCE_ERR_ALREADY_EXISTS) {
        printf("[selftest] terminal/named: failed to register echo app (%d)\n", registered);
        return false;
    }

    int result = terminal__run_line("terminal_test_echo hello \"world now\"");
    if (result <= 0) {
        printf("[selftest] terminal/named: run_line returned %d\n", result);
        return false;
    }

    bruce_result_t wait_result = task__wait((bruce_task_id_t)result, 2000);
    if (wait_result != BRUCE_OK && wait_result != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] terminal/named: wait failed (%d)\n", wait_result);
        return false;
    }

    if (s_echo.argc != 2 || strcmp(s_echo.argv_buf[0], "hello") != 0 ||
        strcmp(s_echo.argv_buf[1], "world now") != 0) {
        printf("[selftest] terminal/named: argc=%d argv=[%s|%s]\n", s_echo.argc, s_echo.argv_buf[0],
               s_echo.argv_buf[1]);
        return false;
    }

    printf("[selftest] terminal/named: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Terminal parser: path dispatch (ELF loader)                               */
/* ------------------------------------------------------------------------ */

bool selftest__run_terminal_path_case(void)
{
    const char *path = "/apps/terminal_test_app.elf";
    storage__remove(path);

    if (!selftest__write_fake_elf(path, "Terminal Test App", NULL, 0)) {
        printf("[selftest] terminal/path: could not create fake ELF\n");
        return false;
    }

    size_t calls_before = elf_loader__debug_call_count();
    int result = terminal__run_line(path);
    size_t calls_after = elf_loader__debug_call_count();

    if (result > 0) {
        (void)task__wait((bruce_task_id_t)result, 2000);
    }
    storage__remove(path);

    if (result <= 0 || calls_after != calls_before + 1) {
        printf("[selftest] terminal/path: ELF loader not dispatched (%d, calls %zu -> %zu)\n", result, calls_before,
               calls_after);
        return false;
    }

    printf("[selftest] terminal/path: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Terminal parser: invalid input                                            */
/* ------------------------------------------------------------------------ */

bool selftest__run_terminal_invalid_case(void)
{
    if (terminal__run_line("") != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] terminal/invalid: empty line did not return BRUCE_ERR_INVALID_ARGUMENT\n");
        return false;
    }
    if (terminal__run_line("   ") != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] terminal/invalid: whitespace-only line did not return BRUCE_ERR_INVALID_ARGUMENT\n");
        return false;
    }
    if (terminal__run_line("selftest_terminal_unknown_command") != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] terminal/invalid: unknown command did not return BRUCE_ERR_NOT_FOUND\n");
        return false;
    }

    printf("[selftest] terminal/invalid: OK\n");
    return true;
}
