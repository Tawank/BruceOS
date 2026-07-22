#include "elf_loader.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/storage.h"

static size_t s_call_count;

size_t elf_loader__debug_call_count(void)
{
    return s_call_count;
}

static bool elf_loader__path_is_valid(const char *path)
{
    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL) {
        return false;
    }
    size_t length = strlen(path);
    static const char extension[] = ".elf";
    size_t extension_length = sizeof(extension) - 1;
    return length > extension_length && strcmp(path + length - extension_length, extension) == 0;
}

static const char *elf_loader__basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

typedef struct {
    char path[BRUCE_STORAGE_PATH_MAX];
    char permission_key[BRUCE_PERMISSION_FILE_NAME_MAX];
} elf_loader_task_ctx_t;

static void elf_loader__app_main(void *context)
{
    elf_loader_task_ctx_t *ctx = (elf_loader_task_ctx_t *)context;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(ctx->path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        uint64_t size = 0;
        if (storage__seek(file, 0, SEEK_END, &size) == BRUCE_OK && size > 0) {
            void *image = memory__malloc((size_t)size);
            if (image != NULL) {
                uint8_t *out = (uint8_t *)image;
                if (storage__seek(file, 0, SEEK_SET, NULL) == BRUCE_OK) {
                    size_t total = 0;
                    while (total < (size_t)size) {
                        size_t chunk = 0;
                        if (storage__read(file, out + total, (size_t)size - total, &chunk) != BRUCE_OK || chunk == 0) {
                            break;
                        }
                        total += chunk;
                    }
                }
                memory__free(image);
            }
        }
        storage__close(file);
    }

    printf("[elf_loader] %s: execution not implemented yet (registry/manifest validation only)\n",
           ctx->permission_key);
    free(ctx);
}

static int elf_loader__run_path(const char *path, const char *arg, bool in_background)
{
    s_call_count++;

    if (!elf_loader__path_is_valid(path)) {
        return BRUCE_ERR_INVALID_PATH;
    }

    bruce_app_inspection_t inspection;
    bruce_result_t inspect_result = manifest__inspect_path(path, &inspection);
    if (inspect_result != BRUCE_OK) {
        return (int)inspect_result;
    }

    const char *permission_key = elf_loader__basename(path);

    const char *permission_names[BRUCE_MANIFEST_MAX_PERMISSIONS];
    for (size_t i = 0; i < inspection.manifest.permission_count; ++i) {
        permission_names[i] = inspection.manifest.permissions[i];
    }
    (void)permission__preflight(permission_key, permission_names, inspection.manifest.permission_count);

    char **argv = NULL;
    int argc = 0;
    bruce_result_t parse_result = app_runner__parse_args(arg, &argv, &argc);
    if (parse_result != BRUCE_OK) {
        return (int)parse_result;
    }
    bool gui_requested = app_runner__args_have_gui(argc, argv);
    app_runner__free_args(argv, argc);

    elf_loader_task_ctx_t *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return BRUCE_ERR_NO_MEMORY;
    }
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    ctx->path[sizeof(ctx->path) - 1] = '\0';
    strncpy(ctx->permission_key, permission_key, sizeof(ctx->permission_key) - 1);
    ctx->permission_key[sizeof(ctx->permission_key) - 1] = '\0';

    int result = app_runner__spawn_loader_task(permission_key, gui_requested, in_background,
                                                inspection.manifest.stack_size, elf_loader__app_main, ctx);
    if (result <= 0) {
        free(ctx);
    }
    return result;
}

void elf_loader__register(void)
{
    (void)app_runner__register_loader(".elf", 10, elf_loader__run_path);
}
