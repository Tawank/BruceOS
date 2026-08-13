/* A6 acceptance coverage: shared manifest parser, the app_runner loader
 * registry's third-party extensibility, and the built-in ELF loader module
 * (see migration_plan.md, "Loader modules"). */
#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core/storage/storage.h"
#include "core_sdk/dialog.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/storage.h"
#include "core_sdk/process.h"
#include "modules/loaders/elf/elf_loader_app.h"

#include "fake_elf.h"
#include "loader_test.h"

/* ------------------------------------------------------------------------ */
/* Shared test helper: base64 of an all-zero 128-byte icon                   */
/* ------------------------------------------------------------------------ */

static void selftest__loader_test_icon_base64(char *out, size_t out_capacity) {
    size_t out_index = 0;
    for (int i = 0; i < 42 && out_index + 4 < out_capacity; ++i) {
        memcpy(out + out_index, "AAAA", 4);
        out_index += 4;
    }
    if (out_index + 4 < out_capacity) {
        memcpy(out + out_index, "AAA=", 4);
        out_index += 4;
    }
    out[out_index] = '\0';
}

/* ------------------------------------------------------------------------ */
/* manifest__parse()                                                         */
/* ------------------------------------------------------------------------ */

bool selftest__run_manifest_parse_case(void) {
    char icon_b64[200];
    selftest__loader_test_icon_base64(icon_b64, sizeof(icon_b64));

    char json[512];
    int len = snprintf(
        json,
        sizeof(json),
        "{\"appName\":\"Example app\",\"appIcon\":\"%s\",\"coreAbiVersion\":2,"
        "\"stackSize\":8192,\"permissions\":[\"wifi\",\"http\"]}",
        icon_b64
    );
    if (len <= 0 || (size_t)len >= sizeof(json)) {
        printf("[selftest] loader/manifest_parse: failed to build test JSON\n");
        return false;
    }

    bruce_manifest_t *manifest = manifest__parse(json, (size_t)len);
    if (manifest == NULL) {
        printf("[selftest] loader/manifest_parse: valid manifest rejected\n");
        return false;
    }
    bool icon_all_zero = true;
    for (size_t i = 0; i < BRUCE_MANIFEST_ICON_BYTES; ++i) {
        if (manifest->app_icon[i] != 0) {
            icon_all_zero = false;
            break;
        }
    }
    bool ok = strcmp(manifest->app_name, "Example app") == 0 && manifest->core_abi_version == 2 &&
              manifest->stack_size == 8192 && manifest->permission_count == 2 &&
              strcmp(manifest->permissions[0], "wifi") == 0 &&
              strcmp(manifest->permissions[1], "http") == 0 && icon_all_zero;
    memory__free(manifest);
    if (!ok) {
        printf("[selftest] loader/manifest_parse: parsed fields mismatch\n");
        return false;
    }

    /* Negative cases: out-of-range stack size, unknown permission name, and
     * a duplicate permission must each be rejected. */
    char bad_stack_json[512];
    snprintf(
        bad_stack_json,
        sizeof(bad_stack_json),
        "{\"appName\":\"x\",\"appIcon\":\"%s\",\"coreAbiVersion\":2,\"stackSize\":100}",
        icon_b64
    );
    if (manifest__parse(bad_stack_json, strlen(bad_stack_json)) != NULL) {
        printf("[selftest] loader/manifest_parse: out-of-range stackSize accepted\n");
        return false;
    }

    char unknown_perm_json[512];
    snprintf(
        unknown_perm_json,
        sizeof(unknown_perm_json),
        "{\"appName\":\"x\",\"appIcon\":\"%s\",\"coreAbiVersion\":2,\"stackSize\":8192,"
        "\"permissions\":[\"not_a_real_permission\"]}",
        icon_b64
    );
    if (manifest__parse(unknown_perm_json, strlen(unknown_perm_json)) != NULL) {
        printf("[selftest] loader/manifest_parse: unknown permission name accepted\n");
        return false;
    }

    char duplicate_perm_json[512];
    snprintf(
        duplicate_perm_json,
        sizeof(duplicate_perm_json),
        "{\"appName\":\"x\",\"appIcon\":\"%s\",\"coreAbiVersion\":2,\"stackSize\":8192,"
        "\"permissions\":[\"wifi\",\"wifi\"]}",
        icon_b64
    );
    if (manifest__parse(duplicate_perm_json, strlen(duplicate_perm_json)) != NULL) {
        printf("[selftest] loader/manifest_parse: duplicate permission accepted\n");
        return false;
    }

    printf("[selftest] loader/manifest_parse: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Loader registry: proves a third-party loader can register a brand new    */
/* extension using only the public API, with no Core changes (A6 accept.).  */
/* ------------------------------------------------------------------------ */

static int s_throwaway_loader_calls;

static int selftest__throwaway_loader_run(
    const char *path, const char *arg, bruce_launch_mode_t mode,
    const bruce_environment_variable_t *environment, size_t environment_count
) {
    (void)path;
    (void)arg;
    (void)mode;
    (void)environment;
    (void)environment_count;
    s_throwaway_loader_calls++;
    return 777; /* sentinel "process id" so the test can prove dispatch happened */
}

bool selftest__run_loader_registry_extensibility_case(void) {
    s_throwaway_loader_calls = 0;

    if (app_runner__register_loader(".selftest_ext", 5, selftest__throwaway_loader_run) != BRUCE_OK) {
        printf("[selftest] loader/registry: registration failed\n");
        return false;
    }
    if (app_runner__register_loader(".selftest_ext", 5, selftest__throwaway_loader_run) !=
        BRUCE_ERR_ALREADY_EXISTS) {
        printf("[selftest] loader/registry: duplicate extension was not rejected\n");
        return false;
    }

    int result = app_runner__run_path("/tmp/whatever.selftest_ext", "some arg", BRUCE_LAUNCH_BACKGROUND);
    if (result != 777 || s_throwaway_loader_calls != 1) {
        printf(
            "[selftest] loader/registry: run_path did not dispatch to the new loader (%d, calls=%d)\n",
            result,
            s_throwaway_loader_calls
        );
        return false;
    }

    if (app_runner__run_path("relative.selftest_ext", "", BRUCE_LAUNCH_BACKGROUND) != BRUCE_ERR_INVALID_PATH) {
        printf("[selftest] loader/registry: relative path was accepted\n");
        return false;
    }
    if (app_runner__run_path("/tmp/whatever.unknown_ext", "", BRUCE_LAUNCH_BACKGROUND) != BRUCE_ERR_NOT_FOUND) {
        printf("[selftest] loader/registry: unknown extension did not return BRUCE_ERR_NOT_FOUND\n");
        return false;
    }

    printf("[selftest] loader/registry: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* Built-in ELF loader module                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile int call_count;
} selftest__elf_dialog_mock_t;

static selftest__elf_dialog_mock_t s_elf_dialog_mock;

static bruce_result_t selftest__elf_dialog_allow_provider(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title;
    (void)message;
    (void)choices;
    (void)choice_count;
    s_elf_dialog_mock.call_count++;
    *out_selected = 0; /* "Allow" (see core/permission/permission.c) */
    return BRUCE_OK;
}

bool selftest__run_elf_loader_case(void) {
    const char *path = "/bin/selftest_elf_loader_target.elf";
    storage__remove(path);

    const char *permissions[] = {"wifi"};
    if (!selftest__write_fake_elf(path, "Selftest ELF", permissions, 1)) {
        printf("[selftest] loader/elf: could not build fake ELF\n");
        return false;
    }

    bruce_app_inspection_t *inspection = manifest__inspect_elf(path);
    bool inspect_ok = inspection != NULL && inspection->kind == BRUCE_APP_KIND_ELF &&
                      !inspection->abi_warning &&
                      strcmp(inspection->manifest.app_name, "Selftest ELF") == 0 &&
                      inspection->manifest.permission_count == 1 &&
                      strcmp(inspection->manifest.permissions[0], "wifi") == 0;
    if (inspection != NULL) { memory__free(inspection); }
    if (!inspect_ok) {
        printf("[selftest] loader/elf: inspect_path mismatch\n");
        storage__remove(path);
        return false;
    }

    dialog__test_set_choice_provider(selftest__elf_dialog_allow_provider);
    size_t calls_before = elf_loader__debug_call_count();
    int result = app_runner__run_path(path, NULL, BRUCE_LAUNCH_BACKGROUND);
    dialog__test_set_choice_provider(NULL);

    bool run_ok = result == BRUCE_ERR_INVALID_ARGUMENT && elf_loader__debug_call_count() == calls_before + 1;
    if (!run_ok) {
        printf("[selftest] loader/elf: invalid fake image was not rejected after staging (%d)\n", result);
        storage__remove(path);
        return false;
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    size_t written = 0;
    bool corrupted = storage__open(path, BRUCE_STORAGE_OPEN_WRITE, &file) == BRUCE_OK &&
                     storage__seek(file, 52, SEEK_SET, NULL) == BRUCE_OK &&
                     storage__write(file, "!", 1, &written) == BRUCE_OK && written == 1;
    if (file != BRUCE_FILE_ID_INVALID) storage__close(file);
    inspection = corrupted ? manifest__inspect_elf(path) : NULL;
    bool fallback_ok = inspection != NULL && inspection->kind == BRUCE_APP_KIND_ELF &&
                       strcmp(inspection->manifest.app_name, "selftest_elf_loader_target") == 0 &&
                       inspection->manifest.stack_size != 0 && inspection->manifest.permission_count == 0;
    if (inspection != NULL) memory__free(inspection);
    if (fallback_ok) {
        result = app_runner__run_path(path, NULL, BRUCE_LAUNCH_BACKGROUND);
        fallback_ok = result == BRUCE_ERR_INVALID_ARGUMENT;
    }
    storage__remove(path);
    if (!fallback_ok) {
        printf("[selftest] loader/elf: invalid manifest did not use fallback metadata\n");
        return false;
    }

    printf("[selftest] loader/elf: OK\n");
    return true;
}

/* ------------------------------------------------------------------------ */
/* WebAssembly manifest custom section                                      */
/* ------------------------------------------------------------------------ */

static size_t selftest__wasm_u32(uint8_t *out, uint32_t value) {
    size_t len = 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7;
        if (value != 0) byte |= 0x80u;
        out[len++] = byte;
    } while (value != 0);
    return len;
}

static bool
selftest__write_wasm(const char *path, const char *manifest, const uint8_t *suffix, size_t suffix_len) {
    static const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    static const char section_name[] = "bruce.manifest";
    uint8_t bytes[768];
    size_t len = 0;
    memcpy(bytes + len, header, sizeof(header));
    len += sizeof(header);
    if (manifest != NULL) {
        size_t manifest_len = strlen(manifest);
        size_t section_size = 1 + sizeof(section_name) - 1 + manifest_len;
        bytes[len++] = 0;
        len += selftest__wasm_u32(bytes + len, (uint32_t)section_size);
        len += selftest__wasm_u32(bytes + len, sizeof(section_name) - 1);
        memcpy(bytes + len, section_name, sizeof(section_name) - 1);
        len += sizeof(section_name) - 1;
        memcpy(bytes + len, manifest, manifest_len);
        len += manifest_len;
    }
    if (len + suffix_len > sizeof(bytes)) { return false; }
    if (suffix_len != 0) memcpy(bytes + len, suffix, suffix_len);
    len += suffix_len;
    return storage__write_file_atomic(path, bytes, len);
}

bool selftest__run_wasm_manifest_case(void) {
    const char *path = "/bin/selftest_manifest.wasm";
    char icon_b64[200];
    char json[512];
    selftest__loader_test_icon_base64(icon_b64, sizeof(icon_b64));
    int json_len = snprintf(
        json,
        sizeof(json),
        "{\"appName\":\"Selftest WASM\",\"appIcon\":\"%s\",\"coreAbiVersion\":%u,"
        "\"stackSize\":8192,\"permissions\":[\"wifi\"]}",
        icon_b64,
        (unsigned)BRUCE_CORE_ABI_VERSION
    );
    if (json_len <= 0 || (size_t)json_len >= sizeof(json) || !selftest__write_wasm(path, json, NULL, 0)) {
        printf("[selftest] loader/wasm_manifest: could not build fixture\n");
        return false;
    }

    char *raw = manifest__inspect_path(path);
    bruce_app_inspection_t *inspection = manifest__inspect_wasm(path);
    bool ok = raw != NULL && strcmp(raw, json) == 0 && inspection != NULL &&
              inspection->kind == BRUCE_APP_KIND_WEBASSEMBLY && !inspection->abi_warning &&
              strcmp(inspection->manifest.app_name, "Selftest WASM") == 0 &&
              inspection->manifest.permission_count == 1;
    memory__free(raw);
    memory__free(inspection);

    if (!selftest__write_wasm(path, "not json", NULL, 0)) ok = false;
    inspection = ok ? manifest__inspect_wasm(path) : NULL;
    ok = ok && inspection != NULL && strcmp(inspection->manifest.app_name, "selftest_manifest") == 0 &&
         inspection->manifest.permission_count == 0;
    memory__free(inspection);

    if (!selftest__write_wasm(path, NULL, NULL, 0)) ok = false;
    inspection = ok ? manifest__inspect_wasm(path) : NULL;
    ok = ok && inspection != NULL && strcmp(inspection->manifest.app_name, "selftest_manifest") == 0;
    memory__free(inspection);

    static const uint8_t bad_leb[] = {0x01, 0x80, 0x80, 0x80, 0x80, 0x10};
    if (!selftest__write_wasm(path, NULL, bad_leb, sizeof(bad_leb))) ok = false;
    inspection = ok ? manifest__inspect_wasm(path) : NULL;
    ok = ok && inspection == NULL;
    memory__free(inspection);

    static const uint8_t truncated_section[] = {0x01, 0x02, 0x00};
    if (!selftest__write_wasm(path, NULL, truncated_section, sizeof(truncated_section))) ok = false;
    inspection = ok ? manifest__inspect_wasm(path) : NULL;
    ok = ok && inspection == NULL;
    memory__free(inspection);

    static const uint8_t bad_version[] = {0x00, 0x61, 0x73, 0x6d, 0x02, 0x00, 0x00, 0x00};
    if (!storage__write_file_atomic(path, bad_version, sizeof(bad_version))) ok = false;
    inspection = ok ? manifest__inspect_wasm(path) : NULL;
    raw = manifest__inspect_path(path);
    ok = ok && inspection == NULL && raw == NULL;
    memory__free(inspection);
    memory__free(raw);
    storage__remove(path);

    printf("[selftest] loader/wasm_manifest: %s\n", ok ? "OK" : "FAILED");
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Built-in JavaScript loader module                                        */
/* ------------------------------------------------------------------------ */

bool selftest__run_js_loader_case(void) {
    const char *path = "/bin/selftest_js_target.js";
    storage__remove(path);

    char icon_b64[200];
    selftest__loader_test_icon_base64(icon_b64, sizeof(icon_b64));

    const char *script_fmt = "/*\n"
                              "{\"appName\":\"Selftest JS\",\"appIcon\":\"%s\",\"coreAbiVersion\":%u,"
                             "\"stackSize\":8192,\"permissions\":[]}\n"
                             "*/\n"
                              "var audio = require('audio');\n"
                              "print(typeof audio.tone);\n"
                              "print('selftest_js_ok');\n";
    char script[512];
    int len = snprintf(script, sizeof(script), script_fmt, icon_b64, (unsigned)BRUCE_CORE_ABI_VERSION);
    if (len <= 0 || (size_t)len >= sizeof(script)) {
        printf("[selftest] loader/js: failed to build test script\n");
        return false;
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK) {
        printf("[selftest] loader/js: could not create test script\n");
        return false;
    }
    size_t written = 0;
    storage__write(file, script, (size_t)len, &written);
    storage__close(file);

    bruce_app_inspection_t *inspection = manifest__inspect_javascript(path);
    bool inspect_ok = inspection != NULL && inspection->kind == BRUCE_APP_KIND_JAVASCRIPT &&
                      !inspection->abi_warning && strcmp(inspection->manifest.app_name, "Selftest JS") == 0 &&
                      inspection->manifest.permission_count == 0;
    if (inspection != NULL) { memory__free(inspection); }
    if (!inspect_ok) {
        printf("[selftest] loader/js: inspect mismatch\n");
        storage__remove(path);
        return false;
    }

    int result = app_runner__run_path(path, NULL, BRUCE_LAUNCH_BACKGROUND);
    bool run_ok = result > 0;
    if (run_ok) { run_ok = (process__wait((bruce_process_id_t)result, 2000) == BRUCE_OK); }
    storage__remove(path);
    if (!run_ok) {
        printf("[selftest] loader/js: run did not spawn or complete (%d)\n", result);
        return false;
    }

    printf("[selftest] loader/js: OK\n");
    return true;
}
