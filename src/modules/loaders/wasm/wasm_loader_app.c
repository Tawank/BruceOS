#include "wasm_loader_app.h"
#include "wasm_bruce_host_adapter.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#define WASM_LOADER_APP_HEAP_BYTES (16u * 1024u)
#define WASM_LOADER_MAX_MEMORY_PAGES 4u
#define WASM_LOADER_MANIFEST_STACK_MAX 16384u

static size_t s_call_count;
static bool s_runtime_initialized;
static StaticSemaphore_t s_runtime_mutex_storage;
static SemaphoreHandle_t s_runtime_mutex;
static portMUX_TYPE s_runtime_mutex_init = portMUX_INITIALIZER_UNLOCKED;

size_t wasm_loader__debug_call_count(void) { return s_call_count; }

typedef struct {
    void *base;
    uint32_t size;
} wasm_loader_allocation_header_t;

_Static_assert(sizeof(wasm_loader_allocation_header_t) == 8, "WAMR allocation header ABI changed");

static void *wasm_loader__runtime_malloc(unsigned int size) {
    if (size > UINT32_MAX - sizeof(wasm_loader_allocation_header_t) - 7u) return NULL;
    void *base = malloc(size + sizeof(wasm_loader_allocation_header_t) + 7u);
    if (base == NULL) {
        printf(
            "[wasm_loader] allocation failed: request=%u free=%u largest=%u\n",
            size,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
        );
        return NULL;
    }
    uintptr_t address = ((uintptr_t)base + sizeof(wasm_loader_allocation_header_t) + 7u) & ~(uintptr_t)7u;
    wasm_loader_allocation_header_t *header = (wasm_loader_allocation_header_t *)address - 1;
    header->base = base;
    header->size = size;
    return (void *)address;
}

static void wasm_loader__runtime_free(void *pointer) {
    if (pointer == NULL) return;
    wasm_loader_allocation_header_t *header = (wasm_loader_allocation_header_t *)pointer - 1;
    free(header->base);
}

static void *wasm_loader__runtime_realloc(void *pointer, unsigned int size) {
    if (pointer == NULL) return wasm_loader__runtime_malloc(size);
    if (size == 0) {
        wasm_loader__runtime_free(pointer);
        return NULL;
    }
    wasm_loader_allocation_header_t *header = (wasm_loader_allocation_header_t *)pointer - 1;
    void *replacement = wasm_loader__runtime_malloc(size);
    if (replacement == NULL) return NULL;
    memcpy(replacement, pointer, header->size < size ? header->size : size);
    wasm_loader__runtime_free(pointer);
    return replacement;
}

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];
    int argc;
    char **argv;
    const uint8_t *module_bytes;
    uint8_t *module_buffer;
    size_t module_size;
    bruce_ext_mem_loader_image_t module_image;
    wasm_module_t module;
    wasm_module_inst_t module_inst;
} wasm_loader_process_ctx_t;

typedef enum {
    WASM_MEMORY_PREFLIGHT_OK,
    WASM_MEMORY_PREFLIGHT_INVALID,
    WASM_MEMORY_PREFLIGHT_LIMIT,
} wasm_memory_preflight_result_t;

static bool wasm_loader__read_u32_leb(const uint8_t **cursor, const uint8_t *end, uint32_t *out) {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (*cursor >= end || shift == 35) return false;
        uint8_t byte = *(*cursor)++;
        if (shift == 28 && (byte & 0x7fu) > 0x0fu) return false;
        value |= (uint32_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return true;
        }
    }
    return false;
}

static bool wasm_loader__skip_bytes(const uint8_t **cursor, const uint8_t *end, size_t count) {
    if (count > (size_t)(end - *cursor)) return false;
    *cursor += count;
    return true;
}

static bool wasm_loader__skip_name(const uint8_t **cursor, const uint8_t *end) {
    uint32_t length;
    return wasm_loader__read_u32_leb(cursor, end, &length) &&
           wasm_loader__skip_bytes(cursor, end, length);
}

static wasm_memory_preflight_result_t wasm_loader__read_memory_limits(
    const uint8_t **cursor, const uint8_t *end, unsigned *memory_count
) {
    uint32_t flags;
    uint32_t initial;
    if (!wasm_loader__read_u32_leb(cursor, end, &flags) ||
        (flags & ~0x07u) != 0 || (flags & 0x06u) != 0 ||
        !wasm_loader__read_u32_leb(cursor, end, &initial)) {
        return WASM_MEMORY_PREFLIGHT_INVALID;
    }
    (*memory_count)++;
    if (initial > WASM_LOADER_MAX_MEMORY_PAGES || *memory_count > 1) {
        return WASM_MEMORY_PREFLIGHT_LIMIT;
    }
    if ((flags & 0x01u) != 0) {
        uint32_t maximum;
        if (!wasm_loader__read_u32_leb(cursor, end, &maximum)) return WASM_MEMORY_PREFLIGHT_INVALID;
        if (maximum > WASM_LOADER_MAX_MEMORY_PAGES || maximum < initial) {
            return WASM_MEMORY_PREFLIGHT_LIMIT;
        }
    }
    return WASM_MEMORY_PREFLIGHT_OK;
}

static wasm_memory_preflight_result_t wasm_loader__preflight_memory(const uint8_t *bytes, size_t size) {
    if (bytes == NULL || size < 8 || memcmp(bytes, "\0asm\1\0\0\0", 8) != 0) {
        return WASM_MEMORY_PREFLIGHT_INVALID;
    }
    const uint8_t *cursor = bytes + 8;
    const uint8_t *end = bytes + size;
    unsigned memory_count = 0;
    while (cursor < end) {
        uint8_t section_id = *cursor++;
        uint32_t section_size;
        if (!wasm_loader__read_u32_leb(&cursor, end, &section_size) ||
            section_size > (size_t)(end - cursor)) return WASM_MEMORY_PREFLIGHT_INVALID;
        const uint8_t *section_end = cursor + section_size;
        bool valid = true;
        wasm_memory_preflight_result_t memory_result = WASM_MEMORY_PREFLIGHT_OK;
        if (section_id == 2) {
            uint32_t import_count;
            valid = wasm_loader__read_u32_leb(&cursor, section_end, &import_count);
            for (uint32_t i = 0; valid && i < import_count; ++i) {
                uint8_t kind;
                valid = wasm_loader__skip_name(&cursor, section_end) &&
                        wasm_loader__skip_name(&cursor, section_end) &&
                        wasm_loader__skip_bytes(&cursor, section_end, 1);
                if (!valid) break;
                kind = cursor[-1];
                if (kind == WASM_IMPORT_EXPORT_KIND_FUNC) {
                    uint32_t type_index;
                    valid = wasm_loader__read_u32_leb(&cursor, section_end, &type_index);
                } else if (kind == WASM_IMPORT_EXPORT_KIND_TABLE) {
                    uint32_t element_type, flags, initial;
                    valid = wasm_loader__read_u32_leb(&cursor, section_end, &element_type) &&
                            wasm_loader__read_u32_leb(&cursor, section_end, &flags) &&
                            wasm_loader__read_u32_leb(&cursor, section_end, &initial);
                    if (valid && (flags & 1u)) valid = wasm_loader__read_u32_leb(&cursor, section_end, &initial);
                } else if (kind == WASM_IMPORT_EXPORT_KIND_MEMORY) {
                    memory_result = wasm_loader__read_memory_limits(&cursor, section_end, &memory_count);
                    valid = memory_result == WASM_MEMORY_PREFLIGHT_OK;
                } else if (kind == WASM_IMPORT_EXPORT_KIND_GLOBAL) {
                    valid = wasm_loader__skip_bytes(&cursor, section_end, 2);
                } else {
                    valid = false;
                }
            }
        } else if (section_id == 5) {
            uint32_t count;
            valid = wasm_loader__read_u32_leb(&cursor, section_end, &count);
            for (uint32_t i = 0; valid && i < count; ++i) {
                memory_result = wasm_loader__read_memory_limits(&cursor, section_end, &memory_count);
                valid = memory_result == WASM_MEMORY_PREFLIGHT_OK;
            }
        }
        if (!valid) {
            return memory_result == WASM_MEMORY_PREFLIGHT_LIMIT ?
                       WASM_MEMORY_PREFLIGHT_LIMIT : WASM_MEMORY_PREFLIGHT_INVALID;
        }
        cursor = section_end;
    }
    return WASM_MEMORY_PREFLIGHT_OK;
}

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
    return length > extension_length && strcasecmp(path + length - extension_length, extension) == 0;
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
    if (s_runtime_initialized) return true;
    if (s_runtime_mutex == NULL) {
        portENTER_CRITICAL(&s_runtime_mutex_init);
        if (s_runtime_mutex == NULL) {
            s_runtime_mutex = xSemaphoreCreateMutexStatic(&s_runtime_mutex_storage);
        }
        portEXIT_CRITICAL(&s_runtime_mutex_init);
    }
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    if (s_runtime_initialized) {
        xSemaphoreGive(s_runtime_mutex);
        return true;
    }

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = wasm_loader__runtime_malloc;
    init_args.mem_alloc_option.allocator.realloc_func = wasm_loader__runtime_realloc;
    init_args.mem_alloc_option.allocator.free_func = wasm_loader__runtime_free;
    if (!wasm_runtime_full_init(&init_args)) {
        printf("[wasm_loader] failed to initialize runtime\n");
        xSemaphoreGive(s_runtime_mutex);
        return false;
    }
    if (!wasm_bruce_host_adapter__register()) {
        printf("[wasm_loader] failed to register Bruce SDK imports\n");
        wasm_runtime_destroy();
        xSemaphoreGive(s_runtime_mutex);
        return false;
    }
    s_runtime_initialized = true;
    xSemaphoreGive(s_runtime_mutex);
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
    free(ctx->module_buffer);
    if (ctx->module_image.memory.handle != 0) (void)ext_mem_loader__release_image(&ctx->module_image);
    free(ctx);
}

static void wasm_loader__cleanup_context(void *context) {
    wasm_loader__free_process_ctx((wasm_loader_process_ctx_t *)context);
}

static int wasm_loader__entry(void *context) {
    wasm_loader_process_ctx_t *ctx = (wasm_loader_process_ctx_t *)context;

    if (ctx->module == NULL || ctx->module_inst == NULL || process__current_signal() != 0) { return BRUCE_ERR_CANCELLED; }

    wasm_function_inst_t main_function = wasm_runtime_lookup_function(ctx->module_inst, "main");
    if (main_function == NULL) return BRUCE_ERR_ABI_MISMATCH;
    if (wasm_func_get_param_count(main_function, ctx->module_inst) != 2 ||
        wasm_func_get_result_count(main_function, ctx->module_inst) != 1) {
        return BRUCE_ERR_ABI_MISMATCH;
    }
    wasm_valkind_t param_types[2];
    wasm_valkind_t result_type[1];
    wasm_func_get_param_types(main_function, ctx->module_inst, param_types);
    wasm_func_get_result_types(main_function, ctx->module_inst, result_type);
    if (param_types[0] != WASM_I32 || param_types[1] != WASM_I32 || result_type[0] != WASM_I32) {
        return BRUCE_ERR_ABI_MISMATCH;
    }

    uint32_t *guest_strings = calloc((size_t)ctx->argc, sizeof(*guest_strings));
    if (guest_strings == NULL) return BRUCE_ERR_NO_MEMORY;
    bool guest_args_ok = true;
    for (int i = 0; i < ctx->argc; ++i) {
        uint8_t *native = NULL;
        guest_strings[i] = wasm_runtime_module_malloc(
            ctx->module_inst, strlen(ctx->argv[i]) + 1u, (void **)&native
        );
        if (guest_strings[i] == 0 || native == NULL) {
            guest_args_ok = false;
            break;
        }
        memcpy(native, ctx->argv[i], strlen(ctx->argv[i]) + 1u);
    }
    uint32_t guest_argv = 0;
    if (guest_args_ok) {
        uint8_t *native = NULL;
        guest_argv = wasm_runtime_module_malloc(
            ctx->module_inst, (size_t)ctx->argc * sizeof(uint32_t), (void **)&native
        );
        if (guest_argv == 0 || native == NULL) guest_args_ok = false;
        else memcpy(native, guest_strings, (size_t)ctx->argc * sizeof(uint32_t));
    }
    if (!guest_args_ok) {
        for (int i = 0; i < ctx->argc; ++i) {
            if (guest_strings[i] != 0) wasm_runtime_module_free(ctx->module_inst, guest_strings[i]);
        }
        free(guest_strings);
        return BRUCE_ERR_NO_MEMORY;
    }
    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(ctx->module_inst, WASM_LOADER_EXEC_STACK_BYTES);
    if (exec_env == NULL) {
        for (int i = 0; i < ctx->argc; ++i) {
            if (guest_strings[i] != 0) wasm_runtime_module_free(ctx->module_inst, guest_strings[i]);
        }
        free(guest_strings);
        return BRUCE_ERR_NO_MEMORY;
    }
    wasm_val_t args[2] = {{.kind = WASM_I32, .of.i32 = ctx->argc},
                          {.kind = WASM_I32, .of.i32 = (int32_t)guest_argv}};
    wasm_val_t result = {.kind = WASM_I32};
    bool executed = wasm_runtime_call_wasm_a(exec_env, main_function, 1, &result, 2, args);
    int exit_code = executed ? result.of.i32 : BRUCE_ERR_INVALID_STATE;
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_module_free(ctx->module_inst, guest_argv);
    for (int i = 0; i < ctx->argc; ++i) {
        if (guest_strings[i] != 0) wasm_runtime_module_free(ctx->module_inst, guest_strings[i]);
    }
    free(guest_strings);

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

    if (ctx->module_image.memory.handle != 0) {
        bruce_result_t adopt_result = ext_mem_loader__adopt_image(&ctx->module_image);
        if (adopt_result != BRUCE_OK) return adopt_result;
        ctx->module_bytes = ctx->module_image.data;
        ctx->module_size = ctx->module_image.size;
    }

    if (!wasm_loader__init_runtime()) {
        return BRUCE_ERR_INVALID_STATE;
    }

    if (ctx->module == NULL) return BRUCE_ERR_INVALID_STATE;

    const InstantiationArgs instantiate_args = {
        .default_stack_size = WASM_LOADER_EXEC_STACK_BYTES,
        .host_managed_heap_size = WASM_LOADER_APP_HEAP_BYTES,
        /* Preflight enforces the declaration limit. Zero preserves WAMR's
         * compacted physical page layout for modules that cannot grow. */
        .max_memory_pages = 0,
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

static int wasm_loader__open(
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

    bool exists = false;
    bruce_result_t exists_result = storage__exists(normalized_path, &exists);
    if (exists_result != BRUCE_OK) { return exists_result; }
    if (!exists) { return BRUCE_ERR_NOT_FOUND; }

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

    bruce_result_t stage_result = ext_mem_loader__stage_path(normalized_path, &ctx->module_image);
    if (stage_result != BRUCE_OK) {
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return stage_result;
    }
    if (ctx->module_image.size > WASM_LOADER_MAX_MODULE_BYTES) {
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    ctx->module_bytes = ctx->module_image.data;
    ctx->module_size = ctx->module_image.size;
    if (ctx->module_image.memory.backend == BRUCE_MEMORY_BACKEND_SWAP) {
        size_t module_size = ctx->module_image.size;
        ctx->module_buffer = malloc(module_size);
        if (ctx->module_buffer == NULL) {
            wasm_loader__free_process_ctx(ctx);
            memory__free(inspection);
            return BRUCE_ERR_NO_MEMORY;
        }
        memcpy(ctx->module_buffer, ctx->module_image.data, module_size);
        (void)ext_mem_loader__release_image(&ctx->module_image);
        ctx->module_bytes = ctx->module_buffer;
        ctx->module_size = module_size;
    }

    char error_buf[128];
    wasm_memory_preflight_result_t memory_result =
        wasm_loader__preflight_memory(ctx->module_bytes, ctx->module_size);
    if (memory_result != WASM_MEMORY_PREFLIGHT_OK) {
        stdio__printf(
            "[wasm_loader] %s: %s\n", wasm_loader__basename(ctx->path),
            memory_result == WASM_MEMORY_PREFLIGHT_LIMIT ?
                "linear memory exceeds the four-page limit" : "invalid linear memory declaration"
        );
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return memory_result == WASM_MEMORY_PREFLIGHT_LIMIT ?
                   BRUCE_ERR_RESOURCE_LIMIT : BRUCE_ERR_INVALID_ARGUMENT;
    }
    ctx->module = wasm_runtime_load(
        (uint8_t *)ctx->module_bytes, (uint32_t)ctx->module_size, error_buf, sizeof(error_buf)
    );
    if (ctx->module == NULL) {
        stdio__printf("[wasm_loader] %s: %s\n", wasm_loader__basename(ctx->path), error_buf);
        wasm_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bool gui_requested = app_runner__environment_requests_gui(environment, environment_count);
    if (inspection->manifest.stack_size > WASM_LOADER_MANIFEST_STACK_MAX) {
        memory__free(inspection);
        wasm_loader__free_process_ctx(ctx);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
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
    int result = wasm_loader__open(path, arg[0] != '\0' ? arg : NULL, mode, NULL, 0);
#if defined(BRUCE_WASM_EXTERNAL_ELF) && BRUCE_WASM_EXTERNAL_ELF
    /* Child callbacks execute from this ELF image, so keep its parent process
     * alive until Core has finished the child and its owned cleanup. */
    if (result > 0) return process__wait((bruce_process_id_t)result, UINT32_MAX);
#endif
    return result;
}
