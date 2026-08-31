#include "app_runner.h"

#include "core/app_runner/app_runner.h"
#include "core/process/process.h"
#include "core/storage/storage.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/filetype.h"
#include "core_sdk/icon.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define APP_RUNNER_MAX_APPS 128
#define APP_RUNNER_PATH_MAX 160
#define APP_RUNNER_MAX_LOADERS 32
#define APP_RUNNER_LOADER_EXTENSION_MAX 16
#define APP_RUNNER_LOADER_PROGRAM_MAX 32

typedef struct {
    const char *name;
    const char *description;
    const char *category;
    bruce_app_entry_t entry;
    uint16_t stack_bytes;
} app_runner_app_t;

static app_runner_app_t s_apps[APP_RUNNER_MAX_APPS];
static int s_app_count;

typedef struct {
    char extension[APP_RUNNER_LOADER_EXTENSION_MAX];
    char program[APP_RUNNER_LOADER_PROGRAM_MAX];
} app_runner_loader_t;

static app_runner_loader_t s_loaders[APP_RUNNER_MAX_LOADERS];
static int s_loader_count;

static bool app_runner__path_has_extension(const char *path, const char *extension);

/* Extension-table lookup only (no I/O, no magic-byte/shebang fallback) -
 * the fast, common-case path for a recognized extension. See filetype.h's
 * detection-order notes for why the shebang fallback below needs the
 * heavier filetype__identify() instead. */
static bool app_runner__config_program_for_extension(const char *path, char *program_out, size_t program_out_size) {
    bruce_filetype_info_t info;
    if (filetype__lookup_extension(path, &info) != BRUCE_OK || info.program[0] == '\0') return false;
    if (strlen(info.program) >= program_out_size) return false;
    strncpy(program_out, info.program, program_out_size - 1);
    program_out[program_out_size - 1] = '\0';
    return true;
}

const char *app_runner__icon_for_path(const char *path) { return filetype__icon_for_path(path); }

static bool app_runner__mode_valid(bruce_launch_mode_t mode) {
    return mode == BRUCE_LAUNCH_FOREGROUND || mode == BRUCE_LAUNCH_BACKGROUND;
}

static bool app_runner__environment_requests_overlay(
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    bool requested = false;
    for (size_t i = 0; i < environment_count; ++i) {
        if (environment[i].name != NULL && strcmp(environment[i].name, "OVERLAY") == 0) {
            requested = environment[i].value != NULL && strcmp(environment[i].value, "1") == 0;
        }
    }
    return requested;
}

bruce_result_t app_runner__register(
    const char *name, const char *description, const char *category, bruce_app_entry_t entry,
    uint32_t stack_bytes
) {
    if (name == NULL || name[0] == '\0' || description == NULL || description[0] == '\0' || entry == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].name, name) == 0) { return BRUCE_ERR_ALREADY_EXISTS; }
    }

    if (s_app_count >= APP_RUNNER_MAX_APPS) { return BRUCE_ERR_RESOURCE_LIMIT; }

    s_apps[s_app_count].name = name;
    s_apps[s_app_count].description = description;
    s_apps[s_app_count].category = category;
    s_apps[s_app_count].entry = entry;
    s_apps[s_app_count].stack_bytes = stack_bytes;
    s_app_count++;
    return BRUCE_OK;
}

size_t app_runner__command_count(void) { return (size_t)s_app_count; }

const char *app_runner__command_name(size_t index) {
    return index < (size_t)s_app_count ? s_apps[index].name : NULL;
}

const char *app_runner__command_description(size_t index) {
    return index < (size_t)s_app_count ? s_apps[index].description : NULL;
}

const char *app_runner__command_category(size_t index) {
    return index < (size_t)s_app_count ? s_apps[index].category : NULL;
}

static bruce_app_entry_t app_runner__find_builtin(const char *app_name) {
    for (int i = 0; i < s_app_count; ++i) {
        if (strcmp(s_apps[i].name, app_name) == 0) { return s_apps[i].entry; }
    }
    return NULL;
}

bruce_result_t app_runner__register_loader(const char *extension, const char *program) {
    if (extension == NULL || extension[0] != '.' || extension[1] == '\0' || program == NULL ||
        program[0] == '\0' || strlen(extension) >= APP_RUNNER_LOADER_EXTENSION_MAX ||
        strlen(program) >= APP_RUNNER_LOADER_PROGRAM_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    for (int i = 0; i < s_loader_count; ++i) {
        if (strcmp(s_loaders[i].extension, extension) == 0) { return BRUCE_ERR_ALREADY_EXISTS; }
    }

    if (s_loader_count >= APP_RUNNER_MAX_LOADERS) { return BRUCE_ERR_RESOURCE_LIMIT; }

    app_runner_loader_t *loader = &s_loaders[s_loader_count++];
    strncpy(loader->extension, extension, APP_RUNNER_LOADER_EXTENSION_MAX - 1);
    loader->extension[APP_RUNNER_LOADER_EXTENSION_MAX - 1] = '\0';
    strncpy(loader->program, program, APP_RUNNER_LOADER_PROGRAM_MAX - 1);
    loader->program[APP_RUNNER_LOADER_PROGRAM_MAX - 1] = '\0';
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

int app_runner__run_path_with_environment(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    if (!app_runner__mode_valid(mode)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!app_runner__path_is_valid(path)) { return BRUCE_ERR_INVALID_PATH; }

    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_EXECUTE);
    if (permission_result != BRUCE_OK) { return permission_result; }

    char normalized_path[APP_RUNNER_PATH_MAX];
    if (!app_runner__normalize_path(path, normalized_path, sizeof(normalized_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }

    app_runner_loader_t *loader = app_runner__find_loader_for_path(normalized_path);
    char configured_program[APP_RUNNER_LOADER_PROGRAM_MAX];
    const char *program = loader != NULL ? loader->program : NULL;
    if (app_runner__config_program_for_extension(
            normalized_path, configured_program, sizeof(configured_program)
        )) {
        program = configured_program;
    }
    if (program == NULL) {
        /* Neither a built-in loader nor extensions.conf claimed this
         * extension (or the path has none) - fall back to sniffing a
         * shebang line, e.g. an extensionless script or one whose
         * extension isn't configured. */
        bruce_filetype_info_t info;
        if (filetype__identify(normalized_path, &info) == BRUCE_OK && !info.is_directory &&
            info.program[0] != '\0' && strlen(info.program) < sizeof(configured_program)) {
            strncpy(configured_program, info.program, sizeof(configured_program) - 1);
            configured_program[sizeof(configured_program) - 1] = '\0';
            program = configured_program;
        }
    }
    if (program == NULL) {
        // printf("app_runner__run_path: normalized_path=%s, loader=NULL\n", normalized_path);
        return BRUCE_ERR_NOT_FOUND;
    }
    char loader_arg[APP_RUNNER_PATH_MAX + 1 + 256];
    int written = snprintf(
        loader_arg,
        sizeof(loader_arg),
        "\"%s\"%s%s",
        normalized_path,
        arg != NULL && arg[0] != '\0' ? " " : "",
        arg != NULL ? arg : ""
    );
    if (written < 0 || (size_t)written >= sizeof(loader_arg)) return BRUCE_ERR_INVALID_ARGUMENT;
    return app_runner__run_with_environment(program, loader_arg, mode, environment, environment_count);
}

int app_runner__run_path(const char *path, const char *arg, bruce_launch_mode_t mode) {
    return app_runner__run_path_with_environment(path, arg, mode, NULL, 0);
}

int app_runner__spawn_loader_process(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context
) {
    return app_runner__spawn_loader_process_owned(
        permission_key, gui_requested, mode, stack_size, environment, environment_count, entry, context, NULL
    );
}

int app_runner__spawn_loader_process_owned(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context, bruce_loader_process_cleanup_fn cleanup
) {
    return app_runner__spawn_loader_process_owned_with_stop(
        permission_key,
        gui_requested,
        mode,
        stack_size,
        environment,
        environment_count,
        entry,
        context,
        cleanup,
        NULL
    );
}

int app_runner__spawn_loader_process_owned_with_stop(
    const char *permission_key, bool gui_requested, bruce_launch_mode_t mode, uint32_t stack_size,
    const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_loader_process_entry_fn entry, void *context, bruce_loader_process_cleanup_fn cleanup,
    bruce_loader_process_stop_fn stop
) {
    if (entry == NULL || !app_runner__mode_valid(mode)) { return BRUCE_ERR_INVALID_ARGUMENT; }
    process_create_params_t params = {
        .name = (permission_key != NULL && permission_key[0] != '\0') ? permission_key : "app",
        .entry = NULL,
        .argc = 0,
        .argv = NULL,
        .built_in = false,
        .gui_requested = gui_requested,
        .permission_key = permission_key,
        .start_in_background = mode == BRUCE_LAUNCH_BACKGROUND,
        .preserve_display = app_runner__environment_requests_overlay(environment, environment_count),
        .environment = environment,
        .environment_count = environment_count,
        .stack_bytes = stack_size,
        .process_entry = entry,
        .process_entry_context = context,
        .process_entry_cleanup = cleanup,
        .process_entry_stop = stop,
    };
    bruce_process_id_t process_id = BRUCE_PROCESS_ID_INVALID;
    bruce_result_t create_result = process_registry__create(&params, &process_id);
    return create_result == BRUCE_OK ? (int)process_id : (int)create_result;
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

/* AppRunner records this process context ahead of any launch-time permission
 * check (see migration_plan.md, "Dialog and process interaction"). */
bool app_runner__environment_requests_gui(
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    bool requested = false;
    for (size_t i = 0; i < environment_count; ++i) {
        if (environment[i].name != NULL && strcmp(environment[i].name, "GUI") == 0) {
            requested = environment[i].value != NULL && strcmp(environment[i].value, "1") == 0;
        }
    }
    return requested;
}

int app_runner__run_with_environment(
    const char *app_name, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    if (!app_runner__mode_valid(mode)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (app_name == NULL || app_name[0] == '\0') { return BRUCE_ERR_INVALID_ARGUMENT; }

    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_EXECUTE);
    if (permission_result != BRUCE_OK) { return permission_result; }

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) { return parse_result; }

    /* 1. registered built-in. */
    bruce_app_entry_t entry = app_runner__find_builtin(app_name);
    // printf("app_runner__run: app_name=%s, entry=%p\n", app_name, (void *)entry);
    // fflush(stdout);
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

        process_create_params_t params = {
            .name = app_name,
            .entry = entry,
            .argc = argc,
            .argv = argv,
            .built_in = true,
            .gui_requested = app_runner__environment_requests_gui(environment, environment_count),
            .start_in_background = mode == BRUCE_LAUNCH_BACKGROUND,
            .preserve_display = app_runner__environment_requests_overlay(environment, environment_count),
            .environment = environment,
            .environment_count = environment_count,
            .stack_bytes = 0,
        };
        for (int i = 0; i < s_app_count; ++i) {
            if (s_apps[i].entry == entry) {
                params.gui_requested = params.gui_requested;
                params.stack_bytes = s_apps[i].stack_bytes;
                break;
            }
        }
        bruce_process_id_t process_id = BRUCE_PROCESS_ID_INVALID;
        bruce_result_t create_result = process_registry__create(&params, &process_id);
        result = create_result == BRUCE_OK ? (int)process_id : (int)create_result;
    } else {
        /* 2. every registered loader, in registration order, matching
         * /bin/<app_name><extension>. */
        result = BRUCE_ERR_NOT_FOUND;
        for (int i = 0; i < s_loader_count; ++i) {
            app_runner_loader_t *loader = &s_loaders[i];
            char path[APP_RUNNER_PATH_MAX];
            int written = snprintf(path, sizeof(path), "/bin/%s%s", app_name, loader->extension);
            if (written < 0 || (size_t)written >= sizeof(path)) { continue; }
            if (storage__exists_internal(path)) {
                char loader_arg[APP_RUNNER_PATH_MAX + 1 + 256];
                int written_arg = snprintf(
                    loader_arg,
                    sizeof(loader_arg),
                    "\"%s\"%s%s",
                    path,
                    arg != NULL && arg[0] != '\0' ? " " : "",
                    arg != NULL ? arg : ""
                );
                if (written_arg < 0 || (size_t)written_arg >= sizeof(loader_arg)) {
                    result = BRUCE_ERR_INVALID_ARGUMENT;
                } else {
                    result = app_runner__run_with_environment(
                        loader->program, loader_arg, mode, environment, environment_count
                    );
                }
                break;
            }
        }
    }

    app_runner__free_args(argv, argc);
    return result;
}

int app_runner__run(const char *app_name, const char *arg, bruce_launch_mode_t mode) {
    return app_runner__run_with_environment(app_name, arg, mode, NULL, 0);
}

static bool app_runner__environment_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0' || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (size_t i = 1; name[i] != '\0'; ++i) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) return false;
    }
    return strlen(name) < BRUCE_ENVIRONMENT_NAME_MAX;
}

static bool app_runner__append_quoted(char *out, size_t capacity, size_t *used, const char *value) {
    size_t needed = *used > 0 ? 1u : 0u;
    needed += 2u;
    for (const char *p = value; *p != '\0'; ++p) needed += (*p == '\\' || *p == '"') ? 2u : 1u;
    if (*used + needed >= capacity) return false;
    if (*used > 0) out[(*used)++] = ' ';
    out[(*used)++] = '"';
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '"') out[(*used)++] = '\\';
        out[(*used)++] = *p;
    }
    out[(*used)++] = '"';
    out[*used] = '\0';
    return true;
}

bruce_result_t app_runner__run_command(const char *command_line, bruce_launch_mode_t default_mode) {
    if (command_line == NULL || !app_runner__mode_valid(default_mode)) return BRUCE_ERR_INVALID_ARGUMENT;
    char **words = NULL;
    int word_count = 0;
    bruce_result_t parsed = app_runner__parse_args(command_line, &words, &word_count);
    if (parsed != BRUCE_OK) return parsed;
    if (word_count == 0) {
        app_runner__free_args(words, word_count);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_environment_variable_t environment[BRUCE_ENVIRONMENT_MAX_VARIABLES];
    size_t environment_count = 0;
    int command_index = 0;
    bruce_launch_mode_t mode = default_mode;
    while (command_index < word_count) {
        char *equals = strchr(words[command_index], '=');
        if (equals == NULL) break;
        *equals = '\0';
        const char *name = words[command_index];
        const char *value = equals + 1;
        if (!app_runner__environment_name_valid(name) || strlen(value) >= BRUCE_ENVIRONMENT_VALUE_MAX ||
            environment_count >= BRUCE_ENVIRONMENT_MAX_VARIABLES) {
            app_runner__free_args(words, word_count);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(name, "BG") == 0) {
            if (strcmp(value, "0") == 0) mode = BRUCE_LAUNCH_FOREGROUND;
            else if (strcmp(value, "1") == 0) mode = BRUCE_LAUNCH_BACKGROUND;
            else {
                app_runner__free_args(words, word_count);
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
        }
        environment[environment_count++] = (bruce_environment_variable_t){.name = name, .value = value};
        command_index++;
    }
    if (command_index >= word_count) {
        app_runner__free_args(words, word_count);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    size_t argument_capacity = strlen(command_line) * 2u + 1u;
    char *arguments = malloc(argument_capacity);
    if (arguments == NULL) {
        app_runner__free_args(words, word_count);
        return BRUCE_ERR_NO_MEMORY;
    }
    arguments[0] = '\0';
    size_t used = 0;
    for (int i = command_index + 1; i < word_count; ++i) {
        if (!app_runner__append_quoted(arguments, argument_capacity, &used, words[i])) {
            free(arguments);
            app_runner__free_args(words, word_count);
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
    }
    const char *command = words[command_index];
    int result;
    if (command[0] == '/' || strncmp(command, "./", 2) == 0) {
        result = app_runner__run_path_with_environment(
            command, used > 0 ? arguments : NULL, mode, environment, environment_count
        );
    } else {
        result = app_runner__run_with_environment(
            command, used > 0 ? arguments : NULL, mode, environment, environment_count
        );
    }
    free(arguments);
    app_runner__free_args(words, word_count);
    return result;
}
