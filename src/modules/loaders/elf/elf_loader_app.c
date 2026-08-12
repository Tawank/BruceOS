#include "elf_loader_app.h"

#include <errno.h> // IWYU pragma: keep
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "esp_elf.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"

static size_t s_call_count;
static char s_missing_symbol[64];

size_t elf_loader__debug_call_count(void) { return s_call_count; }

static bool elf_loader__path_is_valid(const char *path) {
    if (path == NULL || strstr(path, "..") != NULL) { return false; }
    if (path[0] != '/' && strncmp(path, "./", 2) != 0) { return false; }
    size_t length = strlen(path);
    static const char extension[] = ".elf";
    size_t extension_length = sizeof(extension) - 1;
    return length > extension_length && strcmp(path + length - extension_length, extension) == 0;
}

static bool elf_loader__normalize_path(const char *path, char *out, size_t out_size) {
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

static const char *elf_loader__basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static const char *elf_loader__command_name(const char *path) {
    const char *base = elf_loader__basename(path);
    const char *dot = strrchr(base, '.');
    size_t len = dot != NULL ? (size_t)(dot - base) : strlen(base);
    if (len >= BRUCE_STORAGE_NAME_MAX) { len = BRUCE_STORAGE_NAME_MAX - 1; }
    static char name[BRUCE_STORAGE_NAME_MAX];
    memcpy(name, base, len);
    name[len] = '\0';
    return name;
}

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];
    char permission_key[BRUCE_PERMISSION_FILE_NAME_MAX];
    int argc;
    char **argv;
    esp_elf_t elf;
    bool elf_initialized;
    bruce_ext_mem_loader_xip_image_t xip;
} elf_loader_process_ctx_t;

/* Public SDK symbol allowlist. ELF apps may only resolve these names; selected
 * libc names in the table route through process-aware Bruce SDK functions. */
extern const struct esp_elfsym g_bruce_sdk_elfsyms[];

static uintptr_t elf_loader__find_symbol(const char *sym_name) {
    const struct esp_elfsym *syms = g_bruce_sdk_elfsyms;
    while (syms->name != NULL) {
        if (strcmp(syms->name, sym_name) == 0) { return (uintptr_t)syms->sym; }
        syms++;
    }
    if (sym_name != NULL) {
        strncpy(s_missing_symbol, sym_name, sizeof(s_missing_symbol) - 1);
        s_missing_symbol[sizeof(s_missing_symbol) - 1] = '\0';
    }
    return 0;
}

static void elf_loader__free_process_ctx(elf_loader_process_ctx_t *ctx) {
    if (ctx == NULL) { return; }
    if (ctx->elf_initialized) esp_elf_deinit(&ctx->elf);
    app_runner__free_args(ctx->argv, ctx->argc);
    free(ctx);
}

static void elf_loader__cleanup_context(void *context) {
    elf_loader__free_process_ctx((elf_loader_process_ctx_t *)context);
}

/* Process entry for an ELF already relocated by elf_loader__run_path(). */
static void elf_loader__entry(void *context) {
    elf_loader_process_ctx_t *ctx = (elf_loader_process_ctx_t *)context;
    if (ext_mem_loader__adopt_xip(&ctx->xip) == BRUCE_OK) {
        (void)esp_elf_request(&ctx->elf, 0, ctx->argc, ctx->argv);
    }
}

static int elf_loader__xip_allocate(
    void *context, size_t size, const uint8_t **instruction, const uint8_t **data, uint32_t *handle
) {
    elf_loader_process_ctx_t *ctx = context;
    bruce_result_t result = ext_mem_loader__allocate_xip(size, &ctx->xip);
    if (result != BRUCE_OK) return result == BRUCE_ERR_NO_MEMORY ? -ENOMEM : -ENOSPC;
    *instruction = ctx->xip.instruction;
    *data = ctx->xip.data;
    *handle = ctx->xip.memory.handle;
    return 0;
}

static int
elf_loader__xip_write(void *context, uint32_t handle, size_t offset, const void *data, size_t size) {
    elf_loader_process_ctx_t *ctx = context;
    if (ctx->xip.memory.handle != handle) return -EINVAL;
    bruce_result_t result = ext_mem_loader__write_xip(&ctx->xip, offset, data, size);
    if (result != BRUCE_OK) {
        printf(
            "[elf_loader] %s: xip write failed at offset %u size %u (result=%d)\n",
            ctx->permission_key,
            (unsigned)offset,
            (unsigned)size,
            (int)result
        );
    }
    return result == BRUCE_OK ? 0 : (result == BRUCE_ERR_INVALID_ARGUMENT ? -EINVAL : -EIO);
}

static void elf_loader__xip_release(void *context, uint32_t handle) {
    elf_loader_process_ctx_t *ctx = context;
    if (ctx->xip.memory.handle == handle) (void)ext_mem_loader__release_xip(&ctx->xip);
}

static const esp_elf_xip_ops_t s_xip_ops = {
    .allocate = elf_loader__xip_allocate,
    .write = elf_loader__xip_write,
    .release = elf_loader__xip_release,
};

/* Loader registry run function: called by app_runner__run_path() or by the
 * built-in "elf" command. */
int elf_loader__run_path(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    s_call_count++;
    s_missing_symbol[0] = '\0';
    ext_mem_loader__set_error_message(NULL);

    if (!elf_loader__path_is_valid(path)) { return BRUCE_ERR_INVALID_PATH; }

    char normalized_path[BRUCE_STORAGE_PATH_MAX];
    if (!elf_loader__normalize_path(path, normalized_path, sizeof(normalized_path))) {
        return BRUCE_ERR_INVALID_PATH;
    }

    bruce_app_inspection_t *inspection = manifest__inspect_elf(normalized_path);
    if (inspection == NULL) { return BRUCE_ERR_MANIFEST_INVALID; }

    const char *permission_key = elf_loader__basename(normalized_path);

    const char *permission_names[BRUCE_MANIFEST_MAX_PERMISSIONS];
    for (size_t i = 0; i < inspection->manifest.permission_count; ++i) {
        permission_names[i] = inspection->manifest.permissions[i];
    }
    (void)permission__preflight(permission_key, permission_names, inspection->manifest.permission_count);

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) {
        memory__free(inspection);
        return (int)parse_result;
    }

    /* argv[0] is the display name for the loaded ELF app. */
    const char *cmd_name = elf_loader__command_name(normalized_path);
    char **full_argv = NULL;
    int full_argc = argc + 1;
    full_argv = calloc((size_t)full_argc + 1u, sizeof(char *));
    if (full_argv == NULL) {
        app_runner__free_args(argv, argc);
        memory__free(inspection);
        return BRUCE_ERR_NO_MEMORY;
    }

    full_argv[0] = malloc(strlen(cmd_name) + 1);
    if (full_argv[0] == NULL) {
        free(full_argv);
        app_runner__free_args(argv, argc);
        memory__free(inspection);
        return BRUCE_ERR_NO_MEMORY;
    }
    strcpy(full_argv[0], cmd_name);
    for (int i = 0; i < argc; ++i) { full_argv[i + 1] = argv[i]; }
    free(argv);
    argv = NULL;
    argc = 0;

    bool gui_requested = app_runner__environment_requests_gui(environment, environment_count);

    elf_loader_process_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        app_runner__free_args(full_argv, full_argc);
        memory__free(inspection);
        return BRUCE_ERR_NO_MEMORY;
    }
    memset(ctx, 0, sizeof(*ctx));
    strncpy(ctx->path, normalized_path, sizeof(ctx->path) - 1);
    ctx->path[sizeof(ctx->path) - 1] = '\0';
    strncpy(ctx->permission_key, permission_key, sizeof(ctx->permission_key) - 1);
    ctx->permission_key[sizeof(ctx->permission_key) - 1] = '\0';
    ctx->argc = full_argc;
    ctx->argv = full_argv;

    bruce_ext_mem_loader_image_t image;
    bruce_result_t stage_result = ext_mem_loader__stage_path(normalized_path, &image);
    if (stage_result != BRUCE_OK) {
        elf_loader__free_process_ctx(ctx);
        memory__free(inspection);
        return stage_result;
    }
    int relocate_result = esp_elf_init(&ctx->elf);
    if (relocate_result == 0) {
        ctx->elf_initialized = true;
        relocate_result = esp_elf_relocate_xip(&ctx->elf, image.data, image.size, &s_xip_ops, ctx);
    }
    bruce_result_t release_result = ext_mem_loader__release_image(&image);
    if (relocate_result != 0 || release_result != BRUCE_OK) {
        printf(
            "[elf_loader] %s: flash-backed relocation failed (relocate=%d, release=%d)\n",
            ctx->permission_key,
            relocate_result,
            release_result
        );
        elf_loader__free_process_ctx(ctx);
        memory__free(inspection);
        if (s_missing_symbol[0] != '\0') {
            char message[128];
            snprintf(message, sizeof(message), "ELF: Can't find symbol %s", s_missing_symbol);
            ext_mem_loader__set_error_message(message);
            return BRUCE_ERR_ABI_MISMATCH;
        }
        return relocate_result != 0
                   ? (relocate_result == -ENOMEM ? BRUCE_ERR_NO_MEMORY
                                                 : (relocate_result == -ENOSPC ? BRUCE_ERR_RESOURCE_LIMIT
                                                                               : BRUCE_ERR_INVALID_ARGUMENT))
                   : release_result;
    }

    bruce_ext_mem_loader_xip_image_t parent_xip = ctx->xip;
    int result = app_runner__spawn_loader_process_owned(
        permission_key,
        gui_requested,
        mode,
        inspection->manifest.stack_size,
        environment,
        environment_count,
        elf_loader__entry,
        ctx,
        elf_loader__cleanup_context
    );
    if (result <= 0) {
        elf_loader__free_process_ctx(ctx);
    } else {
        for (;;) {
            bruce_process_snapshot_t snapshot;
            bruce_result_t snapshot_result = process__snapshot(result, &snapshot);
            if (snapshot_result != BRUCE_OK) {
                (void)ext_mem_loader__release_xip(&parent_xip);
                break;
            }
            if (snapshot.resource_count > 0) break;
            if (runtime__delay(1) != BRUCE_OK) break;
        }
    }
    memory__free(inspection);
    return result;
}

/* Built-in "elf" command entry: "elf ./target.elf <args>..." loads the
 * named ELF file and passes the remaining arguments to it.  This lets users
 * (and ELF apps themselves) chain loaders: the first loader can be the
 * built-in "elf" command, and a loaded ELF app can call
 * app_runner__run_path() to load another ELF. */
static bool elf_loader__append_arg(char *out, size_t out_size, size_t *used, const char *value) {
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

int elf_loader__app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) { return BRUCE_ERR_NO_MEMORY; }
    ap_set_helptext(parser, "Load and run an ELF application.");
    ap_add_required_arg(parser, "path", "ELF file to load");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP) { return 0; }
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    const char *path = ap_get_arg(parser, "path");

    /* Build the argument string to forward to the loaded ELF. */
    char arg[BRUCE_STORAGE_PATH_MAX];
    size_t arg_len = 0;
    arg[0] = '\0';
    int parsed_argc = ap_count_args(parser);
    for (int i = 1; i < parsed_argc; ++i) {
        const char *forwarded_arg = ap_get_arg_at_index(parser, i);
        if (!elf_loader__append_arg(arg, sizeof(arg), &arg_len, forwarded_arg)) {
            ap_free(parser);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    }
    ap_free(parser);

    /* Inherit the background/foreground mode of the calling "elf" command
     * process so the loaded ELF follows the same context. */
    bruce_process_snapshot_t snapshot;
    bruce_launch_mode_t mode = BRUCE_LAUNCH_FOREGROUND;
    if (process__snapshot(process__current_id(), &snapshot) == BRUCE_OK) {
        mode = snapshot.state == BRUCE_PROCESS_BACKGROUND ? BRUCE_LAUNCH_BACKGROUND : BRUCE_LAUNCH_FOREGROUND;
    }
    return elf_loader__run_path(path, arg[0] != '\0' ? arg : NULL, mode, NULL, 0);
}

void elf_loader__init(void) { elf_set_symbol_resolver(elf_loader__find_symbol); }
