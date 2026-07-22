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

/* Parses and validates canonical manifest JSON bytes (see
 * migration_BruceIDF.md, "ELF contract"): required appName/appIcon (base64,
 * decodes to exactly BRUCE_MANIFEST_ICON_BYTES bytes)/coreAbiVersion/
 * stackSize (4096-16384 inclusive), and an optional permissions array (each
 * name must be a known bruce_permission_t name, no duplicates).  Every
 * caller extracts raw manifest bytes from the file format and calls this one
 * shared parser instead of reimplementing JSON/base64 handling.  Returns
 * BRUCE_OK or BRUCE_ERR_MANIFEST_INVALID. */
bruce_result_t manifest__parse(const char *json, size_t json_len, bruce_manifest_t *out_manifest);

/* Universal manifest inspector (see migration_BruceIDF.md, "Loader
 * modules").  Opens `path`, auto-detects the file format (ELF magic bytes,
 * JS comment block, etc.), extracts the raw manifest bytes, hands them to
 * manifest__parse(), and fills `out_inspection` with the parsed manifest,
 * app kind, and ABI-warning flag.  Never launches an app.  This is the one
 * function every program — loader modules, the launcher, file manager,
 * terminal tools — uses to inspect any file.
 *
 * Returns BRUCE_OK on success, or BRUCE_ERR_INVALID_PATH (null or
 * malformed path), BRUCE_ERR_NOT_FOUND (file does not exist),
 * BRUCE_ERR_MANIFEST_INVALID (missing/invalid manifest), or
 * BRUCE_ERR_TARGET_MISMATCH (ELF e_machine does not match this build). */
bruce_result_t manifest__inspect_path(const char *path, bruce_app_inspection_t *out_inspection);
