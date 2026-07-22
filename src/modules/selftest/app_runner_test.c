#include <stdio.h>
#include <string.h>
#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/task.h"
#include "fake_elf.h"
#include "modules/loaders/elf/elf_loader.h"

/* ------------------------------------------------------------------------ */
/* AppRunner (A3): registration, named resolution, arg parsing, --gui/state */
/* ------------------------------------------------------------------------ */

static int selftest__apprunner_dummy_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

bool selftest__run_apprunner_registration_case(void)
{
    if (app_runner__register("selftest", selftest__apprunner_dummy_entry) != BRUCE_ERR_ALREADY_EXISTS) {
        printf("[selftest] apprunner/registration: duplicate name was not rejected\n");
        return false;
    }
    if (app_runner__register(NULL, selftest__apprunner_dummy_entry) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] apprunner/registration: NULL name was accepted\n");
        return false;
    }
    if (app_runner__run("selftest_apprunner_unregistered_app", "", false) != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] apprunner/registration: unknown app did not return BRUCE_ERR_NOT_FOUND\n");
        return false;
    }
    printf("[selftest] apprunner/registration: OK\n");
    return true;
}

typedef struct {
    volatile int argc;
    char argv_buf[4][32];
    volatile bool gui_requested;
    volatile bruce_task_state_t state;
} selftest__apprunner_echo_t;

static selftest__apprunner_echo_t s_echo;

/* Records its own argc/argv (shell-parsed by AppRunner) and the task state
 * / gui_requested flag AppRunner recorded for it, so the harness can inspect
 * them after the task exits (its own task record is gone by then). */
static int selftest__apprunner_echo_entry(int argc, char **argv)
{
    bruce_task_snapshot_t snapshot;
    if (task__snapshot(task__current_id(), &snapshot) == BRUCE_OK) {
        s_echo.state = snapshot.state;
        s_echo.gui_requested = snapshot.gui_requested;
    }
    s_echo.argc = argc;
    for (int i = 0; i < argc && i < 4; ++i) {
        strncpy(s_echo.argv_buf[i], argv[i], sizeof(s_echo.argv_buf[i]) - 1);
        s_echo.argv_buf[i][sizeof(s_echo.argv_buf[i]) - 1] = '\0';
    }
    return 0;
}

bool selftest__run_apprunner_args_case(void)
{
    bruce_result_t registered = app_runner__register("selftest_echo", selftest__apprunner_echo_entry);
    if (registered != BRUCE_OK && registered != BRUCE_ERR_ALREADY_EXISTS) {
        printf("[selftest] apprunner/args: register failed (%d)\n", registered);
        return false;
    }

    memset(&s_echo, 0, sizeof(s_echo));
    int background_result = app_runner__run("selftest_echo", "--gui foo \"bar baz\" escaped\\ space", true);
    bruce_result_t background_wait = background_result > 0 ? task__wait((bruce_task_id_t)background_result, 2000)
                                                            : BRUCE_ERR_INVALID_ARGUMENT;
    if (background_wait != BRUCE_OK && background_wait != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] apprunner/args: background run did not complete (%d, wait=%d)\n", background_result,
               background_wait);
        return false;
    }
    bool background_ok = s_echo.argc == 4 && strcmp(s_echo.argv_buf[0], "--gui") == 0 &&
                          strcmp(s_echo.argv_buf[1], "foo") == 0 && strcmp(s_echo.argv_buf[2], "bar baz") == 0 &&
                          strcmp(s_echo.argv_buf[3], "escaped space") == 0 && s_echo.gui_requested &&
                          s_echo.state == BRUCE_TASK_BACKGROUND;
    if (!background_ok) {
        printf("[selftest] apprunner/args: background argc=%d gui=%d state=%d argv=[%s|%s|%s|%s]\n", s_echo.argc,
               s_echo.gui_requested, s_echo.state, s_echo.argv_buf[0], s_echo.argv_buf[1], s_echo.argv_buf[2],
               s_echo.argv_buf[3]);
        return false;
    }

    memset(&s_echo, 0, sizeof(s_echo));
    int foreground_result = app_runner__run("selftest_echo", "one two", false);
    bruce_result_t foreground_wait = foreground_result > 0 ? task__wait((bruce_task_id_t)foreground_result, 2000)
                                                            : BRUCE_ERR_INVALID_ARGUMENT;
    if (foreground_wait != BRUCE_OK && foreground_wait != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] apprunner/args: foreground run did not complete (%d, wait=%d)\n", foreground_result,
               foreground_wait);
        return false;
    }
    if (s_echo.argc != 2 || s_echo.gui_requested || s_echo.state != BRUCE_TASK_FOREGROUND) {
        printf("[selftest] apprunner/args: foreground argc=%d gui=%d state=%d\n", s_echo.argc, s_echo.gui_requested,
               s_echo.state);
        return false;
    }

    printf("[selftest] apprunner/args: OK\n");
    return true;
}

#define SELFTEST_APPRUNNER_RESOLUTION_NAME "selftest_apprunner_resolution_target"

bool selftest__run_apprunner_resolution_case(void)
{
    const char *elf_path = "/bin/" SELFTEST_APPRUNNER_RESOLUTION_NAME ".elf";
    const char *js_path = "/bin/" SELFTEST_APPRUNNER_RESOLUTION_NAME ".js";
    storage__remove(elf_path);
    storage__remove(js_path);


    int result = app_runner__run(SELFTEST_APPRUNNER_RESOLUTION_NAME, "", true);

    if (!storage__write_file_atomic(js_path, "js", 2)) {
        printf("[selftest] apprunner/resolution: could not create %s\n", js_path);
        return false;
    }
    result = app_runner__run(SELFTEST_APPRUNNER_RESOLUTION_NAME, "", true);

    /* ELF wins if both exist, and (post-A6) really spawns a task through the
     * loader registry instead of returning a placeholder BRUCE_ERR_*. */
    if (!selftest__write_fake_elf(elf_path, SELFTEST_APPRUNNER_RESOLUTION_NAME ".elf", NULL, 0)) {
        printf("[selftest] apprunner/resolution: could not create %s\n", elf_path);
        storage__remove(js_path);
        return false;
    }
    size_t elf_calls = elf_loader__debug_call_count();
    result = app_runner__run(SELFTEST_APPRUNNER_RESOLUTION_NAME, "", true);
    elf_calls++;
    bool spawned = result > 0 && elf_loader__debug_call_count() == elf_calls;
    if (spawned) {
        (void)task__wait((bruce_task_id_t)result, 2000);
    }
    storage__remove(elf_path);
    storage__remove(js_path);
    if (!spawned) {
        printf("[selftest] apprunner/resolution: both-exist case did not prefer ELF (%d)\n", result);
        return false;
    }

    printf("[selftest] apprunner/resolution: OK\n");
    return true;
}
