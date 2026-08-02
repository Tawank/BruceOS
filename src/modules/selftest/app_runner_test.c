#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/process.h"
#include "fake_elf.h"
#include "modules/loaders/elf/elf_loader_app.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* AppRunner (A3): registration, named resolution, arg parsing, --gui/state */
/* ------------------------------------------------------------------------ */

static int selftest__apprunner_dummy_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

bool selftest__run_apprunner_registration_case(void) {
    size_t command_count = app_runner__command_count();
    bool found_help = false;
    bool found_apps = false;
    for (size_t i = 0; i < command_count; ++i) {
        const char *name = app_runner__command_name(i);
        if (name != NULL && strcmp(name, "help") == 0) found_help = true;
        if (name != NULL && strcmp(name, "apps") == 0) found_apps = true;
    }
    if (!found_help || !found_apps || app_runner__command_name(command_count) != NULL) {
        printf("[selftest] apprunner/registration: command enumeration failed\n");
        return false;
    }
    if (app_runner__register("selftest", selftest__apprunner_dummy_entry, 0) != BRUCE_ERR_ALREADY_EXISTS) {
        printf("[selftest] apprunner/registration: duplicate name was not rejected\n");
        return false;
    }
    if (app_runner__register(NULL, selftest__apprunner_dummy_entry, 0) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] apprunner/registration: NULL name was accepted\n");
        return false;
    }
    if (app_runner__run("selftest_apprunner_unregistered_app", "", BRUCE_LAUNCH_FOREGROUND) != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] apprunner/registration: unknown app did not return BRUCE_ERR_NOT_FOUND\n");
        return false;
    }
    static const char *capacity_names[] = {
        "selftest_apprunner_capacity_1",
        "selftest_apprunner_capacity_2",
    };
    for (size_t i = 0; i < sizeof(capacity_names) / sizeof(capacity_names[0]); ++i) {
        bruce_result_t result = app_runner__register(capacity_names[i], selftest__apprunner_dummy_entry, 0);
        if (result != BRUCE_OK && result != BRUCE_ERR_ALREADY_EXISTS) {
            printf("[selftest] apprunner/registration: additional registration failed (%d)\n", result);
            return false;
        }
    }
    printf("[selftest] apprunner/registration: OK\n");
    return true;
}

typedef struct {
    volatile int argc;
    char argv_buf[5][32];
    volatile bool argv_terminated;
    volatile bool gui_requested;
    volatile bruce_process_state_t state;
    char environment[32];
    char inherited[32];
} selftest__apprunner_echo_t;

static selftest__apprunner_echo_t s_echo;

/* Records its own conventional argc/argv (argv[0] plus shell-parsed arguments)
 * and the process state / gui_requested flag AppRunner recorded for it, so the
 * harness can inspect them after the process exits (its process record is gone by then). */
static int selftest__apprunner_echo_entry(int argc, char **argv) {
    bruce_process_snapshot_t snapshot;
    if (process__snapshot(process__current_id(), &snapshot) == BRUCE_OK) {
        s_echo.state = snapshot.state;
        s_echo.gui_requested = snapshot.gui_requested;
    }
    s_echo.argc = argc;
    const char *environment = environment__get("TEST_VALUE");
    snprintf(s_echo.environment, sizeof(s_echo.environment), "%s", environment != NULL ? environment : "");
    const char *inherited = environment__get("INHERITED_VALUE");
    snprintf(s_echo.inherited, sizeof(s_echo.inherited), "%s", inherited != NULL ? inherited : "");
    if (argc > 1 && strcmp(argv[1], "mutate-environment") == 0) {
        (void)environment__set("INHERITED_VALUE", "child");
    }
    s_echo.argv_terminated = argv != NULL && argv[argc] == NULL;
    for (int i = 0; i < argc && i < 5; ++i) {
        strncpy(s_echo.argv_buf[i], argv[i], sizeof(s_echo.argv_buf[i]) - 1);
        s_echo.argv_buf[i][sizeof(s_echo.argv_buf[i]) - 1] = '\0';
    }
    return 0;
}

bool selftest__run_apprunner_args_case(void) {
    char *lifecycle_argv[] = {"app", "--gui-mode", "--gui", NULL};
    if (!app_runner__args_have_gui(3, lifecycle_argv) || app_runner__args_have_gui(2, lifecycle_argv)) {
        printf("[selftest] apprunner/args: GUI flag was not matched exactly\n");
        return false;
    }
    bruce_result_t registered = app_runner__register("selftest_echo", selftest__apprunner_echo_entry, 0);
    if (registered != BRUCE_OK && registered != BRUCE_ERR_ALREADY_EXISTS) {
        printf("[selftest] apprunner/args: register failed (%d)\n", registered);
        return false;
    }
    if (environment__global_set("TEST_VALUE", "global") != BRUCE_OK ||
        strcmp(environment__global_get("TEST_VALUE"), "global") != 0) {
        printf("[selftest] apprunner/args: global environment setup failed\n");
        return false;
    }

    memset(&s_echo, 0, sizeof(s_echo));
    int background_result = app_runner__run_command(
        "TEST_VALUE=inherited BG=1 selftest_echo --gui foo \"bar baz\" escaped\\ space",
        BRUCE_LAUNCH_FOREGROUND
    );
    bruce_result_t background_wait = background_result > 0
                                         ? process__wait((bruce_process_id_t)background_result, 2000)
                                         : BRUCE_ERR_INVALID_ARGUMENT;
    if (background_wait != BRUCE_OK && background_wait != BRUCE_ERR_NOT_FOUND) {
        printf(
            "[selftest] apprunner/args: background run did not complete (%d, wait=%d)\n",
            background_result,
            background_wait
        );
        return false;
    }
    bool background_ok = s_echo.argc == 5 && strcmp(s_echo.argv_buf[0], "selftest_echo") == 0 &&
                         strcmp(s_echo.argv_buf[1], "--gui") == 0 && strcmp(s_echo.argv_buf[2], "foo") == 0 &&
                         strcmp(s_echo.argv_buf[3], "bar baz") == 0 &&
                         strcmp(s_echo.argv_buf[4], "escaped space") == 0 && s_echo.argv_terminated &&
                          s_echo.gui_requested && s_echo.state == BRUCE_PROCESS_BACKGROUND &&
                          strcmp(s_echo.environment, "inherited") == 0;
    if (!background_ok) {
        printf(
            "[selftest] apprunner/args: background argc=%d gui=%d state=%d argv=[%s|%s|%s|%s|%s]\n",
            s_echo.argc,
            s_echo.gui_requested,
            s_echo.state,
            s_echo.argv_buf[0],
            s_echo.argv_buf[1],
            s_echo.argv_buf[2],
            s_echo.argv_buf[3],
            s_echo.argv_buf[4]
        );
        return false;
    }

    memset(&s_echo, 0, sizeof(s_echo));
    int foreground_result = app_runner__run_command(
        "BG=0 selftest_echo one two", BRUCE_LAUNCH_BACKGROUND
    );
    bruce_result_t foreground_wait = foreground_result > 0
                                         ? process__wait((bruce_process_id_t)foreground_result, 2000)
                                         : BRUCE_ERR_INVALID_ARGUMENT;
    if (foreground_wait != BRUCE_OK && foreground_wait != BRUCE_ERR_NOT_FOUND) {
        printf(
            "[selftest] apprunner/args: foreground run did not complete (%d, wait=%d)\n",
            foreground_result,
            foreground_wait
        );
        return false;
    }
    if (s_echo.argc != 3 || strcmp(s_echo.argv_buf[0], "selftest_echo") != 0 ||
        strcmp(s_echo.argv_buf[1], "one") != 0 || strcmp(s_echo.argv_buf[2], "two") != 0 ||
        !s_echo.argv_terminated || s_echo.gui_requested || s_echo.state != BRUCE_PROCESS_FOREGROUND ||
        strcmp(s_echo.environment, "global") != 0) {
        printf(
            "[selftest] apprunner/args: foreground argc=%d gui=%d state=%d\n",
            s_echo.argc,
            s_echo.gui_requested,
            s_echo.state
        );
        return false;
    }
    (void)environment__global_unset("TEST_VALUE");
    if (app_runner__run_command("BG=invalid selftest_echo", BRUCE_LAUNCH_BACKGROUND) !=
        BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] apprunner/args: invalid BG value was accepted\n");
        return false;
    }

    if (environment__set("INHERITED_VALUE", "parent") != BRUCE_OK) return false;
    memset(&s_echo, 0, sizeof(s_echo));
    int inherited_result = app_runner__run(
        "selftest_echo", "mutate-environment", BRUCE_LAUNCH_BACKGROUND
    );
    if (inherited_result <= 0 || process__wait((bruce_process_id_t)inherited_result, 2000) != BRUCE_OK ||
        strcmp(s_echo.inherited, "parent") != 0 ||
        strcmp(environment__get("INHERITED_VALUE"), "parent") != 0) {
        printf("[selftest] apprunner/args: environment inheritance was not isolated\n");
        return false;
    }
    (void)environment__unset("INHERITED_VALUE");

    printf("[selftest] apprunner/args: OK\n");
    return true;
}

#define SELFTEST_APPRUNNER_RESOLUTION_NAME "selftest_apprunner_resolution_target"

bool selftest__run_apprunner_resolution_case(void) {
    const char *elf_path = "/bin/" SELFTEST_APPRUNNER_RESOLUTION_NAME ".elf";
    const char *js_path = "/bin/" SELFTEST_APPRUNNER_RESOLUTION_NAME ".js";
    storage__remove(elf_path);
    storage__remove(js_path);

    int result = app_runner__run(SELFTEST_APPRUNNER_RESOLUTION_NAME, "", BRUCE_LAUNCH_BACKGROUND);
    if (result != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] apprunner/resolution: missing target returned %d\n", result);
        return false;
    }

    char icon[173];
    memset(icon, 'A', sizeof(icon) - 2);
    icon[sizeof(icon) - 2] = '=';
    icon[sizeof(icon) - 1] = '\0';
    char js_source[384];
    int source_len = snprintf(
        js_source,
        sizeof(js_source),
        "/*\n{\"appName\":\"Resolution Test\",\"appIcon\":\"%s\",\"coreAbiVersion\":2,"
        "\"stackSize\":8192,\"permissions\":[]}\n*/\n",
        icon
    );
    if (source_len <= 0 || (size_t)source_len >= sizeof(js_source) ||
        !storage__write_file_atomic(js_path, js_source, (size_t)source_len)) {
        printf("[selftest] apprunner/resolution: could not create %s\n", js_path);
        return false;
    }
    bool exists = false;
    if (storage__exists(js_path, &exists) != BRUCE_OK || !exists) {
        printf("[selftest] apprunner/resolution: created JS path is not visible\n");
        return false;
    }
    result = app_runner__run(SELFTEST_APPRUNNER_RESOLUTION_NAME, "", BRUCE_LAUNCH_BACKGROUND);
    if (result <= 0) {
        printf("[selftest] apprunner/resolution: JS-only target did not spawn (%d)\n", result);
        storage__remove(js_path);
        return false;
    }
    (void)process__wait((bruce_process_id_t)result, 2000);

    /* ELF wins if both exist, and (post-A6) really spawns a process through the
     * loader registry instead of returning a placeholder BRUCE_ERR_*. */
    if (!selftest__write_fake_elf(elf_path, SELFTEST_APPRUNNER_RESOLUTION_NAME ".elf", NULL, 0)) {
        printf("[selftest] apprunner/resolution: could not create %s\n", elf_path);
        storage__remove(js_path);
        return false;
    }
    size_t elf_calls = elf_loader__debug_call_count();
    result = app_runner__run(SELFTEST_APPRUNNER_RESOLUTION_NAME, "", BRUCE_LAUNCH_BACKGROUND);
    elf_calls++;
    bool spawned = result > 0 && elf_loader__debug_call_count() == elf_calls;
    if (spawned) { (void)process__wait((bruce_process_id_t)result, 2000); }
    storage__remove(elf_path);
    storage__remove(js_path);
    if (!spawned) {
        printf("[selftest] apprunner/resolution: both-exist case did not prefer ELF (%d)\n", result);
        return false;
    }

    printf("[selftest] apprunner/resolution: OK\n");
    return true;
}
