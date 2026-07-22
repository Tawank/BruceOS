#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

#define BRUCE_CORE_ABI_VERSION 1u
#define BRUCE_MANIFEST_APP_NAME_MAX 64
#define BRUCE_MANIFEST_ICON_BYTES 128
#define BRUCE_MANIFEST_MAX_PERMISSIONS 16
#define BRUCE_MANIFEST_PERMISSION_NAME_MAX 16

typedef struct {
    char app_name[BRUCE_MANIFEST_APP_NAME_MAX];
    uint8_t app_icon[BRUCE_MANIFEST_ICON_BYTES];
    uint32_t core_abi_version;
    uint32_t stack_size;
    char permissions[BRUCE_MANIFEST_MAX_PERMISSIONS][BRUCE_MANIFEST_PERMISSION_NAME_MAX];
    size_t permission_count;
} bruce_manifest_t;

typedef enum {
    BRUCE_APP_KIND_ELF,
    BRUCE_APP_KIND_JAVASCRIPT,
} bruce_app_kind_t;

typedef struct {
    bruce_app_kind_t kind;
    bruce_manifest_t manifest;
    int abi_warning;
} bruce_app_inspection_t;

/* Inspection never launches an app.  It returns BRUCE_OK or
 * BRUCE_ERR_NOT_FOUND, BRUCE_ERR_INVALID_PATH, BRUCE_ERR_MANIFEST_INVALID,
 * BRUCE_ERR_TARGET_MISMATCH, or another BRUCE_ERR_* result. */
bruce_result_t elf__inspect_path(const char *path, bruce_app_inspection_t *out_inspection);
bruce_result_t js__inspect_path(const char *path, bruce_app_inspection_t *out_inspection);
