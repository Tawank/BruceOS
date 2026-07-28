#include "app_runner.h"

#include "core/app_runner/app_runner.h"
#include "core/storage/storage.h"
#include "core/task/task.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"

#include "modules/bluetooth/bluetooth_app.h"
#include "modules/bluetooth_hid/bluetooth_hid_app.h"
#include "modules/bnu/bnu_app.h"
#include "modules/bruce_launcher/bruce_launcher_app.h"
#include "modules/filemanager/filemanager_app.h"
#include "modules/ir/ir_app.h"
#include "modules/loaders/elf/elf_loader_app.h"
#include "modules/loaders/image/image_loader_app.h"
#include "modules/loaders/js/js_loader_app.h"
#include "modules/nrf24/nrf24_app.h"
#include "modules/selftest/selftest.h"
#include "modules/tcp/tcp_app.h"
#include "modules/utils/launcher/launcher_app.h"
#include "modules/utils/notification/notification_app.h"
#include "modules/utils/serial_commands/serial_commands_app.h"
#include "modules/utils/terminal/terminal_app.h"
#include "modules/webui/webui_app.h"
#include "modules/wifi/wifi_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define APP_RUNNER_MAX_APPS 32
#define APP_RUNNER_PATH_MAX 160
#define APP_RUNNER_MAX_LOADERS 12
#define APP_RUNNER_SELFTEST_STACK_BYTES 8192u
#define APP_RUNNER_LOADER_EXTENSION_MAX 16

typedef struct {
    const char *name;
    bruce_app_entry_t entry;
    bool gui_default;
    uint32_t stack_bytes;
} app_runner_app_t;

static app_runner_app_t s_apps[APP_RUNNER_MAX_APPS];
static int s_app_count;

typedef struct {
    char extension[APP_RUNNER_LOADER_EXTENSION_MAX];
    int priority;
    bruce_loader_run_fn run_fn;
} app_runner_loader_t;

static app_runner_loader_t s_loaders[APP_RUNNER_MAX_LOADERS];
static int s_loader_count;

bruce_result_t app_runner__register(const char *name, bruce_app_entry_t entry) {
    if (name == NULL || name[0] == '\0' || entry == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }

    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].name, name) == 0) { return BRUCE_ERR_ALREADY_EXISTS; }
    }

    if (s_app_count >= APP_RUNNER_MAX_APPS) { return BRUCE_ERR_RESOURCE_LIMIT; }

    s_apps[s_app_count].name = name;
    s_apps[s_app_count].entry = entry;
    s_apps[s_app_count].gui_default = false;
    s_apps[s_app_count].stack_bytes = 0;
    s_app_count++;
    return BRUCE_OK;
}

void app_runner__register_defaults(void) {
    if (s_app_count != 0) return;

    (void)app_runner__register("launcher", launcher_app_main);
    (void)app_runner__register("bruce_launcher", bruce_launcher_app_main);
    (void)app_runner__register("filemanager", filemanager_app_main);
    (void)app_runner__register("wifi", wifi_app_main);
    (void)app_runner__register("webui", webui_app_main);
    (void)app_runner__register("bluetooth", bluetooth_app_main);
    (void)app_runner__register("bluetooth_hid_app", bluetooth_hid_app_main);
    (void)app_runner__register("ir", ir_app_main);
    (void)app_runner__register("nrf24", nrf24_app_main);
    (void)app_runner__register("selftest", selftest_app_main);
    s_apps[s_app_count - 1].stack_bytes = APP_RUNNER_SELFTEST_STACK_BYTES;
    (void)app_runner__register("terminal", terminal_app_main);
    s_apps[s_app_count - 1].gui_default = true;
    (void)app_runner__register("serial_commands", serial_commands_app_main);
    (void)app_runner__register("pwd", bnu_pwd_app_main);
    (void)app_runner__register("cd", bnu_cd_app_main);
    (void)app_runner__register("ls", bnu_ls_app_main);
    (void)app_runner__register("free", bnu_free_app_main);
    (void)app_runner__register("top", bnu_top_app_main);
    (void)app_runner__register("mkdir", bnu_mkdir_app_main);
    (void)app_runner__register("touch", bnu_touch_app_main);
    (void)app_runner__register("elf", elf_loader__app_main);
    (void)app_runner__register("js", js_loader__app_main);
    (void)app_runner__register("image", image_app_main);
    (void)app_runner__register("image_viewer", image_viewer_app_main);
    (void)app_runner__register("notification", notification_app_main);
    (void)app_runner__register("tcp", tcp_app_main);

    (void)app_runner__register_loader(".elf", 10, elf_loader__run_path);
    (void)app_runner__register_loader(".js", 20, js_loader__run_path);
    (void)app_runner__register_loader(".jpg", 30, image_loader__run_path);
    (void)app_runner__register_loader(".jpeg", 30, image_loader__run_path);
    (void)app_runner__register_loader(".png", 30, image_loader__run_path);
    (void)app_runner__register_loader(".gif", 30, image_loader__run_path);

    elf_loader__init();
}

static bruce_app_entry_t app_runner__find_builtin(const char *app_name) {
    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].name, app_name) == 0) { return s_apps[i].entry; }
    }
    return NULL;
}

bruce_result_t app_runner__register_loader(const char *extension, int priority, bruce_loader_run_fn run_fn) {
    if (extension == NULL || extension[0] != '.' || extension[1] == '\0' || run_fn == NULL ||
        strlen(extension) >= APP_RUNNER_LOADER_EXTENSION_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    for (int i = 0; i < s_loader_count; ++i) {
        if (strcmp(s_loaders[i].extension, extension) == 0) { return BRUCE_ERR_ALREADY_EXISTS; }
    }

    if (s_loader_count >= APP_RUNNER_MAX_LOADERS) { return BRUCE_ERR_RESOURCE_LIMIT; }

    app_runner_loader_t *loader = &s_loaders[s_loader_count++];
    strncpy(loader->extension, extension, APP_RUNNER_LOADER_EXTENSION_MAX - 1);
    loader->extension[APP_RUNNER_LOADER_EXTENSION_MAX - 1] = '\0';
    loader->priority = priority;
    loader->run_fn = run_fn;
    return BRUCE_OK;
}

static bool app_runner__path_is_valid(const char *path) {
    if (path == NULL || strstr(path, "..") != NULL) { return false; }
    return path[0] == '/' || strncmp(path, "./", 2) == 0;
}

static bool app_runner__normalize_path(const char *path, char *out, size_t out_size) {
    if (path == NULL || strstr(path, "..") != NULL || out_size == 0) { return false; }
    int len;
    if (path[0] == '/') {
        len = snprintf(out, out_size, "%s", path);
    } else if (strncmp(path, "./", 2) == 0) {
        len = snprintf(out, out_size, "/%s", path + 2);
    } else {
        return false;
    }
    return len > 0 && (size_t)len < out_size;
}

static bool app_runner__path_has_extension(const char *path, const char *extension) {
    size_t path_len = strlen(path);
    size_t ext_len = strlen(extension);
    return path_len > ext_len && strcasecmp(path + path_len - ext_len, extension) == 0;
}

static app_runner_loader_t *app_runner__find_loader_for_path(const char *path) {
    for (int i = 0; i < s_loader_count; ++i) {
        if (app_runner__path_has_extension(path, s_loaders[i].extension)) { return &s_loaders[i]; }
    }
    return NULL;
}

/* Fills `order` (capacity APP_RUNNER_MAX_LOADERS) with s_loaders indices
 * sorted by ascending priority (stable for equal priorities). Returns the
 * loader count. */
static int app_runner__loader_priority_order(int *order) {
    for (int i = 0; i < s_loader_count; ++i) { order[i] = i; }
    for (int i = 1; i < s_loader_count; ++i) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && s_loaders[order[j]].priority > s_loaders[key].priority) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
    return s_loader_count;
}

int app_runner__run_path(const char *path, const char *arg, bool in_background) {
    if (!app_runner__path_is_valid(path)) { return BRUCE_ERR_INVALID_PATH; }

    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_EXECUTE);
    if (permission_result != BRUCE_OK) { return permission_result; }

    char normalized_path[APP_RUNNER_PATH_MAX];
    if (!app_runner__normalize_path(path, normalized_path, sizeof(normalized_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }

    app_runner_loader_t *loader = app_runner__find_loader_for_path(normalized_path);
    if (loader == NULL) {
        printf("app_runner__run_path: normalized_path=%s, loader=NULL\n", normalized_path);
        return BRUCE_ERR_NOT_FOUND;
    }
    printf(
        "app_runner__run_path: normalized_path=%s, loader=%p, priority=%d\n",
        normalized_path,
        (void *)loader,
        loader->priority
    );
    return loader->run_fn(normalized_path, arg, in_background);
}

int app_runner__spawn_loader_task(
    const char *permission_key, bool gui_requested, bool in_background, uint32_t stack_size,
    bruce_loader_task_entry_fn entry, void *context
) {
    if (entry == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    task_create_params_t params = {
        .name = (permission_key != NULL && permission_key[0] != '\0') ? permission_key : "app",
        .entry = NULL,
        .argc = 0,
        .argv = NULL,
        .built_in = false,
        .gui_requested = gui_requested,
        .permission_key = permission_key,
        .start_in_background = in_background,
        .stack_bytes = stack_size,
        .task_entry = entry,
        .task_entry_context = context,
    };
    bruce_task_id_t task_id = BRUCE_TASK_ID_INVALID;
    bruce_result_t create_result = task_registry__create(&params, &task_id);
    return create_result == BRUCE_OK ? (int)task_id : (int)create_result;
}

void app_runner__free_args(char **argv, int argc) {
    if (argv == NULL) { return; }
    for (int i = 0; i < argc; ++i) { free(argv[i]); }
    free(argv);
}

bruce_result_t app_runner__parse_args(const char *arg, char ***out_argv, int *out_argc) {
    *out_argv = NULL;
    *out_argc = 0;
    if (arg == NULL || arg[0] == '\0') { return BRUCE_OK; }

    size_t length = strlen(arg);
    char *token = malloc(length + 1);
    if (token == NULL) { return BRUCE_ERR_NO_MEMORY; }

    char **argv = NULL;
    int argc = 0;
    size_t i = 0;

    while (i < length) {
        while (i < length && (arg[i] == ' ' || arg[i] == '\t')) { i++; }
        if (i >= length) { break; }

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
            if (c == ' ' || c == '\t') { break; }
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
                    app_runner__free_args(argv, argc);
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
            app_runner__free_args(argv, argc);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }

        token[token_len] = '\0';
        char **grown = realloc(argv, sizeof(char *) * (size_t)(argc + 1));
        if (grown == NULL) {
            free(token);
            app_runner__free_args(argv, argc);
            return BRUCE_ERR_NO_MEMORY;
        }
        argv = grown;
        argv[argc] = malloc(token_len + 1);
        if (argv[argc] == NULL) {
            free(token);
            app_runner__free_args(argv, argc);
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
 * check (see migration_plan.md, "Dialog and task interaction"); the
 * "--gui" token is left in argv, not stripped. */
bool app_runner__args_have_gui(int argc, char *const *argv) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i] != NULL && strcmp(argv[i], "--gui") == 0) { return true; }
    }
    return false;
}

int app_runner__run(const char *app_name, const char *arg, bool in_background) {
    if (app_name == NULL || app_name[0] == '\0') { return BRUCE_ERR_INVALID_ARGUMENT; }

    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_EXECUTE);
    if (permission_result != BRUCE_OK) { return permission_result; }

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) { return parse_result; }

    /* 1. registered built-in. */
    bruce_app_entry_t entry = app_runner__find_builtin(app_name);
    printf("app_runner__run: app_name=%s, entry=%p\n", app_name, (void *)entry);
    fflush(stdout);
    int result;
    if (entry != NULL) {
        char *command_name = malloc(strlen(app_name) + 1u);
        if (command_name == NULL) {
            app_runner__free_args(argv, argc);
            return BRUCE_ERR_NO_MEMORY;
        }
        strcpy(command_name, app_name);
        char **full_argv = realloc(argv, ((size_t)argc + 1u) * sizeof(*full_argv));
        if (full_argv == NULL) {
            free(command_name);
            app_runner__free_args(argv, argc);
            return BRUCE_ERR_NO_MEMORY;
        }
        argv = full_argv;
        memmove(argv + 1, argv, (size_t)argc * sizeof(*argv));
        argv[0] = command_name;
        argc++;

        task_create_params_t params = {
            .name = app_name,
            .entry = entry,
            .argc = argc,
            .argv = argv,
            .built_in = true,
            .gui_requested = app_runner__args_have_gui(argc, argv),
            .start_in_background = in_background,
            .stack_bytes = 0,
        };
        for (int i = 0; i < s_app_count; ++i) {
            if (s_apps[i].entry == entry) {
                params.gui_requested = params.gui_requested || s_apps[i].gui_default;
                params.stack_bytes = s_apps[i].stack_bytes;
                break;
            }
        }
        bruce_task_id_t task_id = BRUCE_TASK_ID_INVALID;
        bruce_result_t create_result = task_registry__create(&params, &task_id);
        result = create_result == BRUCE_OK ? (int)task_id : (int)create_result;
    } else {
        /* 2. every registered loader, tried in ascending priority order,
         * matching /bin/<app_name><extension>.  Core ships ELF at priority
         * 10 and JS at priority 20, so ELF still wins if both exist; a
         * third-party loader slots in the same way at its own priority. */
        int order[APP_RUNNER_MAX_LOADERS];
        int loader_count = app_runner__loader_priority_order(order);
        result = BRUCE_ERR_NOT_FOUND;
        for (int i = 0; i < loader_count; ++i) {
            app_runner_loader_t *loader = &s_loaders[order[i]];
            char path[APP_RUNNER_PATH_MAX];
            int written = snprintf(path, sizeof(path), "/bin/%s%s", app_name, loader->extension);
            if (written < 0 || (size_t)written >= sizeof(path)) { continue; }
            if (storage__exists(path)) {
                result = loader->run_fn(path, arg, in_background);
                break;
            }
        }
    }

    app_runner__free_args(argv, argc);
    return result;
}
