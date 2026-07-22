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

/* Inspection never launches an app.  A loader module implements this per
 * migration_BruceIDF.md, "ELF contract" / "JavaScript contract", and
 * registers it with app_runner__register_loader(); callers reach it through
 * the loader-agnostic app_runner__inspect_path() (core_sdk/loader.h), not by
 * name.  It returns BRUCE_OK or BRUCE_ERR_NOT_FOUND, BRUCE_ERR_INVALID_PATH,
 * BRUCE_ERR_MANIFEST_INVALID, BRUCE_ERR_TARGET_MISMATCH, or another
 * BRUCE_ERR_* result. */

/* Parses and validates canonical manifest JSON bytes (see
 * migration_BruceIDF.md, "ELF contract"): required appName/appIcon (base64,
 * decodes to exactly BRUCE_MANIFEST_ICON_BYTES bytes)/coreAbiVersion/
 * stackSize (4096-16384 inclusive), and an optional permissions array (each
 * name must be a known bruce_permission_t name, no duplicates).  Every
 * loader module (ELF, JavaScript, or a future format) extracts its own raw
 * manifest bytes - an ELF section, a leading JS comment block, or whatever a
 * new format uses - and calls this one shared parser instead of
 * reimplementing JSON/base64 handling.  Returns BRUCE_OK or
 * BRUCE_ERR_MANIFEST_INVALID. */
bruce_result_t manifest__parse(const char *json, size_t json_len, bruce_manifest_t *out_manifest);
