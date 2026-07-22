#include "app_runner.h"

#include "core/app_runner/app_runner.h"
#include "core/storage/storage.h"
#include "core/task/task.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/elf.h"
#include "core_sdk/js.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"
#include "modules/bruce_launcher/bruce_launcher.h"
#include "modules/selftest/selftest.h"
#include "modules/utils/launcher/launcher.h"
#include "modules/wifi/wifi_app.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define APP_RUNNER_MAX_APPS 8
#define APP_RUNNER_PATH_MAX 160

typedef struct {
    const char *name;
    bruce_app_entry_t entry;
} app_runner_app_t;

static app_runner_app_t s_apps[APP_RUNNER_MAX_APPS];
static int s_app_count;

bruce_result_t app_runner__register(const char *name, bruce_app_entry_t entry)
{
    if (name == NULL || name[0] == '\0' || entry == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].name, name) == 0) {
            return BRUCE_ERR_ALREADY_EXISTS;
        }
    }

    if (s_app_count >= APP_RUNNER_MAX_APPS) {
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    s_apps[s_app_count].name = name;
    s_apps[s_app_count].entry = entry;
    s_app_count++;
    return BRUCE_OK;
}

void app_runner__register_defaults(void)
{
    if (s_app_count != 0) {
        return;
    }

    (void)app_runner__register("launcher", launcher_app);
    (void)app_runner__register("bruce_launcher", bruce_launcher_app);
    (void)app_runner__register("wifi", wifi_app);
    (void)app_runner__register("selftest", selftest_app);
}

static bruce_app_entry_t app_runner__find_builtin(const char *app_name)
{
    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].name, app_name) == 0) {
            return s_apps[i].entry;
        }
    }
    return NULL;
}

static void app_runner__free_argv(char **argv, int argc)
{
    if (argv == NULL) {
        return;
    }
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);
}

/* Shell-style tokenizer for `arg`: splits on runs of spaces/tabs; supports
 * single quotes (fully literal, no escapes recognized inside), double
 * quotes (backslash escapes only `\"` and `\\`, everything else literal),
 * and a bare backslash outside quotes to escape the following character.
 * NULL or an empty `arg` produces argc == 0 with *out_argv left NULL.
 * Returns BRUCE_ERR_INVALID_ARGUMENT for an unterminated quote or a trailing
 * unescaped backslash, and BRUCE_ERR_NO_MEMORY on allocation failure. */
static bruce_result_t app_runner__parse_args(const char *arg, char ***out_argv, int *out_argc)
{
    *out_argv = NULL;
    *out_argc = 0;
    if (arg == NULL || arg[0] == '\0') {
        return BRUCE_OK;
    }

    size_t length = strlen(arg);
    char *token = malloc(length + 1);
    if (token == NULL) {
        return BRUCE_ERR_NO_MEMORY;
    }

    char **argv = NULL;
    int argc = 0;
    size_t i = 0;

    while (i < length) {
        while (i < length && (arg[i] == ' ' || arg[i] == '\t')) {
            i++;
        }
        if (i >= length) {
            break;
        }

        size_t token_len = 0;
        bool in_single = false;
        bool in_double = false;

        while (i < length) {
            char c = arg[i];
            if (in_single) {
                if (c == '\'') {
                    in_single = false;
                    i++;
                    continue;
                }
                token[token_len++] = c;
                i++;
                continue;
            }
            if (in_double) {
                if (c == '"') {
                    in_double = false;
                    i++;
                    continue;
                }
                if (c == '\\' && i + 1 < length && (arg[i + 1] == '"' || arg[i + 1] == '\\')) {
                    token[token_len++] = arg[i + 1];
                    i += 2;
                    continue;
                }
                token[token_len++] = c;
                i++;
                continue;
            }
            if (c == ' ' || c == '\t') {
                break;
            }
            if (c == '\'') {
                in_single = true;
                i++;
                continue;
            }
            if (c == '"') {
                in_double = true;
                i++;
                continue;
            }
            if (c == '\\') {
                if (i + 1 >= length) {
                    free(token);
                    app_runner__free_argv(argv, argc);
                    return BRUCE_ERR_INVALID_ARGUMENT;
                }
                token[token_len++] = arg[i + 1];
                i += 2;
                continue;
            }
            token[token_len++] = c;
            i++;
        }

        if (in_single || in_double) {
            free(token);
            app_runner__free_argv(argv, argc);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }

        token[token_len] = '\0';
        char **grown = realloc(argv, sizeof(char *) * (size_t)(argc + 1));
        if (grown == NULL) {
            free(token);
            app_runner__free_argv(argv, argc);
            return BRUCE_ERR_NO_MEMORY;
        }
        argv = grown;
        argv[argc] = malloc(token_len + 1);
        if (argv[argc] == NULL) {
            free(token);
            app_runner__free_argv(argv, argc);
            return BRUCE_ERR_NO_MEMORY;
        }
        memcpy(argv[argc], token, token_len + 1);
        argc++;
    }

    free(token);
    *out_argv = argv;
    *out_argc = argc;
    return BRUCE_OK;
}

/* AppRunner records this task context ahead of any launch-time permission
 * check (see migration_BruceIDF.md, "Dialog and task interaction"); the
 * "--gui" token is left in argv, not stripped. */
static bool app_runner__argv_has_gui(int argc, char **argv)
{
    for (int i = 0; i < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], "--gui") == 0) {
            return true;
        }
    }
    return false;
}

int app_runner__run(const char *app_name, const char *arg, bool in_background)
{
    if (app_name == NULL || app_name[0] == '\0') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) {
        return parse_result;
    }

    /* 1. registered built-in. */
    bruce_app_entry_t entry = app_runner__find_builtin(app_name);
    int result;
    if (entry != NULL) {
        task_create_params_t params = {
            .name = app_name,
            .entry = entry,
            .argc = argc,
            .argv = argv,
            .built_in = true,
            .gui_requested = app_runner__argv_has_gui(argc, argv),
            .start_in_background = in_background,
            .stack_bytes = 0,
        };
        bruce_task_id_t task_id = BRUCE_TASK_ID_INVALID;
        bruce_result_t create_result = task_registry__create(&params, &task_id);
        result = create_result == BRUCE_OK ? (int)task_id : (int)create_result;
    } else {
        /* 2. /bin/<app_name>.elf, then 3. /bin/<app_name>.js; ELF wins if
         * both exist.  Neither existing is BRUCE_ERR_NOT_FOUND. */
        char path[APP_RUNNER_PATH_MAX];
        int written = snprintf(path, sizeof(path), "/bin/%s.elf", app_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else if (storage__exists(path)) {
            result = elf__run_path(path, arg, in_background);
        } else {
            written = snprintf(path, sizeof(path), "/bin/%s.js", app_name);
            if (written < 0 || (size_t)written >= sizeof(path)) {
                result = BRUCE_ERR_INVALID_ARGUMENT;
            } else if (storage__exists(path)) {
                result = js__run_path(path, arg, in_background);
            } else {
                result = BRUCE_ERR_NOT_FOUND;
            }
        }
    }

    app_runner__free_argv(argv, argc);
    return result;
}

