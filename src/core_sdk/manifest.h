#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core_sdk/permission.h"

#define BRUCE_CORE_ABI_VERSION 4u
#define BRUCE_MANIFEST_APP_NAME_MAX 64
#define BRUCE_MANIFEST_ICON_BYTES 128
#define BRUCE_MANIFEST_MAX_PERMISSIONS BRUCE_PERMISSION_COUNT
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
    BRUCE_APP_KIND_WEBASSEMBLY,
} bruce_app_kind_t;

typedef struct {
    bruce_app_kind_t kind;
    bruce_manifest_t manifest;
    int abi_warning;
} bruce_app_inspection_t;

/* Parses and validates canonical manifest JSON bytes (see
 * migration_plan.md, "ELF contract"): required appName/appIcon (base64,
 * decodes to exactly BRUCE_MANIFEST_ICON_BYTES bytes)/coreAbiVersion/
 * stackSize (4096-16384 inclusive), and an optional permissions array (each
 * name must be a known bruce_permission_t name, no duplicates).  Every
 * caller extracts raw manifest bytes from the file format and calls this one
 * shared parser instead of reimplementing JSON/base64 handling. Returns a
 * process-owned manifest that must be released with memory__free(), or NULL
 * for invalid input or allocation failure. */
bruce_manifest_t *manifest__parse(const char *json, size_t json_len);

/* Universal manifest JSON extractor (see migration_plan.md, "Loader
 * modules").  Opens `path`, auto-detects the file format (ELF section, JS
 * comment block, or WebAssembly `bruce.manifest` custom section), and returns
 * raw NUL-terminated manifest JSON. The
 * returned buffer is process-owned and must be released with memory__free(). Never launches an
 * app.  This is the one function every program — the launcher, file
 * manager, terminal tools — uses to extract the manifest from any file.
 *
 * Does NOT parse the JSON or do format-specific validation (e_machine,
 * ABI warning).  Callers that need a parsed manifest call manifest__parse()
 * on the returned bytes.  Format-specific full inspection is handled by
 * manifest__inspect_elf() and similar per-format functions.
 *
 * Returns NULL for an invalid path, inaccessible file, unsupported format,
 * missing manifest, or allocation failure. */
char *manifest__inspect_path(const char *path);

/* ELF-specific manifest inspection (see migration_plan.md, "ELF
 * contract").  Opens `path`, validates the ELF32 header (magic, e_machine
 * vs. this build's target), extracts and parses the .bruce.manifest
 * section via manifest__parse(), and fills *out_inspection with the parsed
 * manifest, BRUCE_APP_KIND_ELF, and the ABI-warning flag. A valid ELF with a
 * missing or invalid manifest receives fallback metadata: its filename,
 * current Core ABI, an 8192-byte stack, and no predeclared permissions. Loader modules
 * that know they are loading an ELF file (the built-in ELF loader, for
 * example) call this directly.
 *
 * The returned inspection is process-owned and must be released with
 * memory__free(). Returns NULL for invalid paths, missing files, malformed ELF
 * headers, target mismatches, or allocation failures. */
bruce_app_inspection_t *manifest__inspect_elf(const char *path);

/* JavaScript-specific manifest inspection (see migration_plan.md,
 * "JavaScript contract").  Opens `path`, detects a leading block comment
 * (slash-asterisk ... asterisk-slash) containing the canonical manifest JSON,
 * and parses it.
 * Fills *out_inspection with the parsed manifest, BRUCE_APP_KIND_JAVASCRIPT,
 * and ABI-warning flag.  If there is no leading manifest block, the script
 * still gets a valid inspection using its filename as the app name, a generic
 * icon, the current ABI version, and zero permissions.
 *
 * The returned inspection is process-owned and must be released with
 * memory__free(). Returns NULL for invalid paths, inaccessible files, or
 * allocation failures. */
bruce_app_inspection_t *manifest__inspect_javascript(const char *path);

/* WebAssembly-specific manifest inspection. Opens a `.wasm` path, validates
 * the standard WebAssembly magic and version, safely walks bounded u32 LEB128
 * section lengths, and parses the payload of a `bruce.manifest` custom section
 * as canonical manifest JSON. The returned inspection has
 * BRUCE_APP_KIND_WEBASSEMBLY and an ABI-warning flag.
 *
 * A structurally valid WebAssembly module with a missing, duplicate, oversized,
 * or invalid manifest receives filename fallback metadata with a generic icon,
 * current Core ABI, an 8192-byte stack, and no permissions. Malformed modules,
 * invalid paths, inaccessible files, and allocation failures return NULL. The
 * returned inspection is process-owned and must be released with
 * memory__free(). */
bruce_app_inspection_t *manifest__inspect_wasm(const char *path);
