#pragma once

#include <stddef.h>
#include <stdint.h>

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
bruce_manifest_t *manifest__parse(const char *json, size_t json_len);

/* Universal manifest JSON extractor (see migration_BruceIDF.md, "Loader
 * modules").  Opens `path`, auto-detects the file format (ELF section, JS
 * comment block, etc.), and returns the raw manifest JSON bytes in
 * *out_json (malloc'd, caller must free with free()).  Never launches an
 * app.  This is the one function every program — the launcher, file
 * manager, terminal tools — uses to extract the manifest from any file.
 *
 * Does NOT parse the JSON or do format-specific validation (e_machine,
 * ABI warning).  Callers that need a parsed manifest call manifest__parse()
 * on the returned bytes.  Format-specific full inspection is handled by
 * manifest__inspect_elf() and similar per-format functions.
 *
 * Returns BRUCE_OK on success, BRUCE_ERR_INVALID_PATH, BRUCE_ERR_NOT_FOUND,
 * or BRUCE_ERR_MANIFEST_INVALID (no extractable manifest in this file). */
const char *manifest__inspect_path(const char *path);

/* ELF-specific manifest inspection (see migration_BruceIDF.md, "ELF
 * contract").  Opens `path`, validates the ELF32 header (magic, e_machine
 * vs. this build's target), extracts and parses the .bruce.manifest
 * section via manifest__parse(), and fills *out_inspection with the parsed
 * manifest, BRUCE_APP_KIND_ELF, and the ABI-warning flag.  Loader modules
 * that know they are loading an ELF file (the built-in ELF loader, for
 * example) call this directly.
 *
 * Returns BRUCE_OK on success, or BRUCE_ERR_INVALID_PATH, BRUCE_ERR_NOT_FOUND,
 * BRUCE_ERR_MANIFEST_INVALID, BRUCE_ERR_TARGET_MISMATCH. */
bruce_app_inspection_t *manifest__inspect_elf(const char *path);

/* JavaScript-specific manifest inspection (see migration_BruceIDF.md,
 * "JavaScript contract").  Opens `path`, detects a leading block comment
 * (slash-asterisk ... asterisk-slash) containing the canonical manifest JSON,
 * and parses it.
 * Fills *out_inspection with the parsed manifest, BRUCE_APP_KIND_JAVASCRIPT,
 * and ABI-warning flag.  If there is no leading manifest block, the script
 * still gets a valid inspection using its filename as the app name, a generic
 * icon, the current ABI version, and zero permissions.
 *
 * Returns BRUCE_OK on success, or BRUCE_ERR_INVALID_PATH, BRUCE_ERR_NOT_FOUND,
 * BRUCE_ERR_MANIFEST_INVALID. */
bruce_app_inspection_t *manifest__inspect_javascript(const char *path);
