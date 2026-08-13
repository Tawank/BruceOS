#include "wasm_loader_app.h"
#include "wasm_bruce_sdk.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "wasm_export.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

#define WASM_LOADER_MAX_MODULE_BYTES (1024u * 1024u)
#define WASM_LOADER_EXEC_STACK_BYTES 8192u
#define WASM_LOADER_APP_HEAP_BYTES (64u * 1024u)
#define WASM_LOADER_MAX_MEMORY_PAGES 4u

static size_t s_call_count;
static bool s_runtime_initialized;

size_t wasm_loader__debug_call_count(void) { return s_call_count; }

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];
    int argc;
    char **argv;
    uint8_t *module_bytes;
    size_t module_size;
    wasm_module_t module;
    wasm_module_inst_t module_inst;
} wasm_loader_process_ctx_t;

static const char *wasm_loader__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static const char *wasm_loader__command_name(const char *path) {
    const char *base = wasm_loader__basename(path);
    const char *dot = strrchr(base, '.');
    size_t len = dot != NULL ? (size_t)(dot - base) : strlen(base);
    if (len >= BRUCE_STORAGE_NAME_MAX) { len = BRUCE_STORAGE_NAME_MAX - 1; }
    static char name[BRUCE_STORAGE_NAME_MAX];
    memcpy(name, base, len);
    name[len] = '\0';
    return name;
}

static bool wasm_loader__path_is_valid(const char *path) {
    if (path == NULL || strstr(path, "..") != NULL) { return false; }
    if (path[0] != '/' && strncmp(path, "./", 2) != 0) { return false; }
    size_t length = strlen(path);
    static const char extension[] = ".wasm";
    size_t extension_length = sizeof(extension) - 1;
    return length > extension_length && strcmp(path + length - extension_length, extension) == 0;
}

static bool wasm_loader__normalize_path(const char *path, char *out, size_t out_size) {
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

static bool wasm_loader__init_runtime(void) {
    if (s_runtime_initialized) { return true; }

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = malloc;
    init_args.mem_alloc_option.allocator.realloc_func = realloc;
    init_args.mem_alloc_option.allocator.free_func = free;
    if (!wasm_runtime_full_init(&init_args)) {
        printf("[wasm_loader] failed to initialize runtime\n");
        return false;
    }
    if (!wasm_bruce_sdk__register()) {
        printf("[wasm_loader] failed to register Bruce SDK imports\n");
        wasm_runtime_destroy();
        return false;
    }
    s_runtime_initialized = true;
    return true;
}

void wasm_loader__init(void) {
    (void)wasm_loader__init_runtime();
}

static void wasm_loader__free_process_ctx(wasm_loader_process_ctx_t *ctx) {
    if (ctx == NULL) { return; }
    if (ctx->module_inst != NULL) wasm_runtime_deinstantiate(ctx->module_inst);
    if (ctx->module != NULL) wasm_runtime_unload(ctx->module);
    app_runner__free_args(ctx->argv, ctx->argc);
    free(ctx->module_bytes);
    free(ctx);
}

static void wasm_loader__cleanup_context(void *context) {
    wasm_loader__free_process_ctx((wasm_loader_process_ctx_t *)context);
}

static int wasm_loader__entry(void *context) {
    wasm_loader_process_ctx_t *ctx = (wasm_loader_process_ctx_t *)context;

    if (ctx->module == NULL || ctx->module_inst == NULL || process__current_signal() != 0) { return BRUCE_ERR_CANCELLED; }

    char **execution_argv = calloc((size_t)ctx->argc + 1u, sizeof(char *));
    if (execution_argv == NULL) return BRUCE_ERR_NO_MEMORY;
    memcpy(execution_argv, ctx->argv, (size_t)ctx->argc * sizeof(char *));
    char *original_argv0 = execution_argv[0];
    bool executed = wasm_application_execute_main(ctx->module_inst, ctx->argc, execution_argv);
    int exit_code = BRUCE_ERR_INVALID_STATE;
    if (executed) {
        /* WAMR stores a returned i32 in argv[0]; a void main leaves it alone. */
        exit_code = execution_argv[0] == original_argv0 ? 0 : (int)(uintptr_t)execution_argv[0];
    }
    free(execution_argv);

    if (!executed &&
        process__current_signal() == 0) {
        const char *exception = wasm_runtime_get_exception(ctx->module_inst);
        stdio__printf(
            "[wasm_loader] %s: %s\n",
            wasm_loader__basename(ctx->path),
            exception != NULL ? exception : "execution failed"
        );
    }
    return exit_code;
}

static void wasm_loader__stop(void *context, bruce_process_signal_t signal) {
    (void)signal;
    wasm_loader_process_ctx_t *ctx = (wasm_loader_process_ctx_t *)context;
    if (ctx != NULL && ctx->module_inst != NULL) wasm_runtime_terminate(ctx->module_inst);
}

static int wasm_loader__process_entry(void *context) {
    wasm_loader_process_ctx_t *ctx = (wasm_loader_process_ctx_t *)context;
    char error_buf[128];

    if (!wasm_loader__init_runtime()) {
        return BRUCE_ERR_INVALID_STATE;
    }

    if (ctx->module_bytes == NULL || ctx->module_size == 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    ctx->module = wasm_runtime_load(ctx->module_bytes, (uint32_t)ctx->module_size, error_buf, sizeof(error_buf));
    if (ctx->module == NULL) {
        stdio__printf("[wasm_loader] %s: %s\n", wasm_loader__basename(ctx->path), error_buf);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    const InstantiationArgs instantiate_args = {
        .default_stack_size = WASM_LOADER_EXEC_STACK_BYTES,
        .host_managed_heap_size = WASM_LOADER_APP_HEAP_BYTES,
        .max_memory_pages = WASM_LOADER_MAX_MEMORY_PAGES,
    };
    ctx->module_inst = wasm_runtime_instantiate_ex(
        ctx->module, &instantiate_args, error_buf, sizeof(error_buf)
    );
    if (ctx->module_inst == NULL) {
        stdio__printf("[wasm_loader] %s: %s\n", wasm_loader__basename(ctx->path), error_buf);
        return BRUCE_ERR_NO_MEMORY;
    }

    return wasm_loader__entry(ctx);
}

int wasm_loader__run_path(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    s_call_count++;
    if (!wasm_loader__path_is_valid(path)) { return BRUCE_ERR_INVALID_PATH; }

    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!wasm_loader__normalize_path(path, normalized_path, sizeof(normalized_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }

    if (!wasm_loader__init_runtime()) { return BRUCE_ERR_INVALID_STATE; }

    bruce_app_inspection_t *inspection = manifest__inspect_wasm(normalized_path);
    if (inspection == NULL) { return BRUCE_ERR_MANIFEST_INVALID; }

    const char *permission_key = wasm_loader__basename(normalized_path);
    const char *permission_names[BRUCE_MANIFEST_MAX_PERMISSIONS];
    for (size_t i = 0; i < inspection->manifest.permission_count; ++i) {
        permission_names[i] = inspection->manifest.permissions[i];
    }
    bruce_result_t preflight_result = permission__preflight(
        permission_key, permission_names, inspection->manifest.permission_count
    );
    if (preflight_result != BRUCE_OK) {
        memory__free(inspection);
        return preflight_result;
    }

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) {
        memory__free(inspection);
        return (int)parse_result;
    }

    char **full_argv = calloc((size_t)argc + 2u, sizeof(char *));
    if (full_argv == NULL) {
        app_runner__free_args(argv, argc);
        memory__free(inspection);
        return BRUCE_ERR_NO_MEMORY;
    }

    const char *cmd_name = wasm_loader__command_name(normalized_path);
    full_argv[0] = malloc(strlen(cmd_name) + 1u);
    if (full_argv[0] == NULL) {
        free(full_argv);
        app_runner__free_args(argv, argc);
        memory__free(inspection);
        return BRUCE_ERR_NO_MEMORY;
    }
    strcpy(full_argv[0], cmd_name);
    for (int i = 0; i < argc; ++i) { full_argv[i + 1] = argv[i]; }
    free(argv);

    wasm_loader_process_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        app_runner__free_args(full_argv, argc + 1);
        memory__free(inspection);
        return BRUCE_ERR_NO_MEMORY;
    }
    strncpy(ctx->path, normalized_path, sizeof(ctx->path) - 1u);
    ctx->argc = argc + 1;
    ctx->argv = full_argv;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result = storage__open(normalized_path, BRUCE_STORAGE_OPEN_READ, &file);
    if (open_result != BRUCE_OK) {
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return open_result;
    }

    uint64_t file_size = 0;
    bruce_result_t seek_result = storage__seek(file, 0, SEEK_END, &file_size);
    if (seek_result != BRUCE_OK) {
        storage__close(file);
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return seek_result;
    }
    if (file_size == 0) {
        storage__close(file);
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (file_size > WASM_LOADER_MAX_MODULE_BYTES || file_size >= SIZE_MAX) {
        storage__close(file);
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    if (file_size > 0) {
        ctx->module_bytes = malloc((size_t)file_size);
        if (ctx->module_bytes == NULL) {
            storage__close(file);
            wasm_loader__free_process_ctx(ctx);
            memory__free(inspection);
            return BRUCE_ERR_NO_MEMORY;
        }
        if (storage__seek(file, 0, SEEK_SET, NULL) != BRUCE_OK) {
            storage__close(file);
            wasm_loader__free_process_ctx(ctx);
            memory__free(inspection);
            return BRUCE_ERR_IO;
        }
        size_t total = 0;
        while (total < (size_t)file_size) {
            size_t chunk = 0;
            bruce_result_t read_result = storage__read(file, ctx->module_bytes + total, (size_t)file_size - total, &chunk);
            if (read_result != BRUCE_OK || chunk == 0) {
                storage__close(file);
                wasm_loader__free_process_ctx(ctx);
                memory__free(inspection);
                return read_result == BRUCE_OK ? BRUCE_ERR_IO : read_result;
            }
            total += chunk;
        }
        ctx->module_size = (size_t)file_size;
    }
    storage__close(file);
    if (ctx->module_bytes == NULL || ctx->module_size == 0) {
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return BRUCE_ERR_NOT_FOUND;
    }

    bool gui_requested = app_runner__environment_requests_gui(environment, environment_count);
    int process_id = app_runner__spawn_loader_process_owned_with_stop(
        permission_key,
        gui_requested,
        mode,
        inspection->manifest.stack_size,
        environment,
        environment_count,
        wasm_loader__process_entry,
        ctx,
        wasm_loader__cleanup_context,
        wasm_loader__stop
    );
    if (process_id < 0) {
        wasm_loader__free_process_ctx(ctx);
    }
    memory__free(inspection);
    return process_id;
}

static bool wasm_loader__append_arg(char *out, size_t out_size, size_t *used, const char *value) {
    size_t needed = *used > 0 ? 1u : 0u;
    needed += 2;
    for (const char *p = value; *p != '\0'; ++p) needed += (*p == '\\' || *p == '"') ? 2u : 1u;
    if (needed >= out_size - *used) return false;
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

int wasm_loader__app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Open a WebAssembly module.");
    ap_add_required_arg(parser, "path", "Path to a .wasm module");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    char path[BRUCE_STORAGE_PATH_MAX];
    const char *parsed_path = ap_get_arg(parser, "path");
    if (parsed_path == NULL || snprintf(path, sizeof(path), "%s", parsed_path) >= (int)sizeof(path)) {
        ap_free(parser);
        return BRUCE_ERR_INVALID_PATH;
    }

    char arg[BRUCE_STORAGE_PATH_MAX];
    size_t arg_len = 0;
    arg[0] = '\0';
    int parsed_argc = ap_count_args(parser);
    for (int i = 1; i < parsed_argc; ++i) {
        if (!wasm_loader__append_arg(
                arg, sizeof(arg), &arg_len, ap_get_arg_at_index(parser, i)
            )) {
            ap_free(parser);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    }
    ap_free(parser);

    bruce_process_snapshot_t snapshot;
    bruce_launch_mode_t mode = BRUCE_LAUNCH_FOREGROUND;
    if (process__snapshot(process__current_id(), &snapshot) == BRUCE_OK &&
        snapshot.state == BRUCE_PROCESS_BACKGROUND) {
        mode = BRUCE_LAUNCH_BACKGROUND;
    }
    return wasm_loader__run_path(path, arg[0] != '\0' ? arg : NULL, mode, NULL, 0);
}
