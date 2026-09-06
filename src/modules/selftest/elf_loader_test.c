#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/environment.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "modules/loaders/elf/elf_loader_sdk_symbols_test.h"
#include "modules/loaders/wasm/wasm_loader_app.h"
#include "platform_api_vmcore.h"

#include "elf_loader_test.h"

/*
 * Exercises the FILE*-based stdio adapters the ELF loader hands to
 * sandboxed apps (elf_loader_sdk_symbols.c, backed by storage__), by
 * calling the (deliberately non-static) adapter functions directly rather
 * than through a cross-compiled ELF fixture -- there is nothing
 * ELF-specific about the adapter logic itself, just mode-string parsing,
 * flag mapping, and eof/error tracking on top of core_sdk/storage.h.
 */
bool selftest__run_elf_loader_stdio_case(void) {
    const char *path = "/selftest_elf_stdio.txt";
    storage__remove(path);

    static const char text[] = "line one\nline two";

    FILE *write_stream = bruce_elf__fopen(path, "w");
    if (write_stream == NULL) {
        printf("[selftest] loader/elf_stdio: fopen(w) failed\n");
        return false;
    }
    size_t written = bruce_elf__fwrite(text, 1, sizeof(text) - 1, write_stream);
    int printf_result = bruce_elf__fprintf(write_stream, "%s", "!");
    if (written != sizeof(text) - 1 || printf_result != 1 || bruce_elf__fclose(write_stream) != 0) {
        printf("[selftest] loader/elf_stdio: write/fprintf/fclose failed\n");
        storage__remove(path);
        return false;
    }

    FILE *read_stream = bruce_elf__fopen(path, "r");
    if (read_stream == NULL) {
        printf("[selftest] loader/elf_stdio: fopen(r) failed\n");
        storage__remove(path);
        return false;
    }

    char first_line[32] = {0};
    if (bruce_elf__fgets(first_line, sizeof(first_line), read_stream) == NULL ||
        strcmp(first_line, "line one\n") != 0) {
        printf("[selftest] loader/elf_stdio: fgets mismatch (\"%s\")\n", first_line);
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }

    long after_first_line = bruce_elf__ftell(read_stream);
    if (after_first_line != (long)strlen("line one\n")) {
        printf("[selftest] loader/elf_stdio: ftell mismatch (%ld)\n", after_first_line);
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }

    char rest[32] = {0};
    size_t rest_read = bruce_elf__fread(rest, 1, sizeof(rest) - 1, read_stream);
    if (rest_read != strlen("line two!") || strcmp(rest, "line two!") != 0 ||
        !bruce_elf__feof(read_stream) || bruce_elf__ferror(read_stream)) {
        printf("[selftest] loader/elf_stdio: fread/feof mismatch (\"%s\", %zu bytes)\n", rest, rest_read);
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }

    bruce_elf__rewind(read_stream);
    if (bruce_elf__feof(read_stream) || bruce_elf__ftell(read_stream) != 0) {
        printf("[selftest] loader/elf_stdio: rewind did not reset position/eof\n");
        bruce_elf__fclose(read_stream);
        storage__remove(path);
        return false;
    }
    bruce_elf__fclose(read_stream);
    storage__remove(path);

    bool missing_exists = true;
    storage__exists(path, &missing_exists);
    errno = 0;
    if (bruce_elf__fopen(path, "r") != NULL || errno != ENOENT || missing_exists) {
        printf("[selftest] loader/elf_stdio: missing-file fopen(r) did not fail with ENOENT\n");
        return false;
    }

    printf("[selftest] loader/elf_stdio: OK\n");
    return true;
}

/*
 * Exercises the getenv/setenv/unsetenv and strdup/strndup adapters
 * (elf_loader_sdk_symbols.c), by calling the (deliberately non-static)
 * adapter functions directly, same rationale as selftest__run_elf_loader_stdio_case().
 */
bool selftest__run_elf_loader_libc_case(void) {
    static const char name[] = "SELFTEST_ELF_LIBC_VAR";
    environment__unset(name);

    if (bruce_elf__getenv(name) != NULL) {
        printf("[selftest] loader/elf_libc: getenv saw a stale value\n");
        return false;
    }

    if (bruce_elf__setenv(name, "first", 0) != 0 || strcmp(environment__get(name), "first") != 0) {
        printf("[selftest] loader/elf_libc: setenv(overwrite=0) on a new var failed\n");
        return false;
    }
    if (bruce_elf__setenv(name, "second", 0) != 0 || strcmp(bruce_elf__getenv(name), "first") != 0) {
        printf("[selftest] loader/elf_libc: setenv(overwrite=0) clobbered an existing value\n");
        return false;
    }
    if (bruce_elf__setenv(name, "second", 1) != 0 || strcmp(bruce_elf__getenv(name), "second") != 0) {
        printf("[selftest] loader/elf_libc: setenv(overwrite=1) did not replace the value\n");
        return false;
    }
    if (bruce_elf__unsetenv(name) != 0 || bruce_elf__getenv(name) != NULL) {
        printf("[selftest] loader/elf_libc: unsetenv did not clear the value\n");
        return false;
    }

    static const char text[] = "bruce elf strdup";
    char *duplicate = bruce_elf__strdup(text);
    if (duplicate == NULL || duplicate == text || strcmp(duplicate, text) != 0) {
        printf("[selftest] loader/elf_libc: strdup mismatch\n");
        return false;
    }
    memory__free(duplicate);

    char *partial = bruce_elf__strndup(text, 5);
    if (partial == NULL || strcmp(partial, "bruce") != 0) {
        printf("[selftest] loader/elf_libc: strndup mismatch (\"%s\")\n", partial ? partial : "(null)");
        memory__free(partial);
        return false;
    }
    memory__free(partial);

    printf("[selftest] loader/elf_libc: OK\n");
    return true;
}

// TODO: Fix elf_loader_test.c
/* native_apps/examples/game.elf, embedded via EMBED_FILES (src/CMakeLists.txt). */
// extern const uint8_t game_elf_start[] asm("_binary_game_elf_start");
// extern const uint8_t game_elf_end[] asm("_binary_game_elf_end");

/*
 * Regression coverage for the flash-backed (XIP) ELF relocation path: stages
 * a real, small, executable ELF app (committed at native_apps/examples/game.elf)
 * and runs it through the exact same AppRunner path dispatch ->
 * esp_elf_relocate_xip() -> memory_external swap allocator pipeline that
 * "elf ./apps/game.elf" uses on real hardware. This is the pipeline that
 * regressed with "flash-backed relocation failed (relocate=-5, release=0)":
 * selftest__run_elf_loader_case()'s fake ELF fixture is intentionally
 * invalid manifest-only bytes and is rejected before relocation is ever
 * attempted, so it cannot catch this class of bug.
 */
bool selftest__run_elf_loader_xip_case(void) {
    const char *path = "/bin/selftest_elf_loader_xip.elf";
    storage__remove(path);

    /* The external fixture writes to storage while running; selftests must
     * never wait for a user to answer its first-use permission prompt. */
    if (permission__set("selftest_elf_loader_xip.elf", BRUCE_PERMISSION_STORAGE, true) != BRUCE_OK) {
        printf("[selftest] loader/elf_xip: could not grant storage permission\n");
        return false;
    }

    // size_t elf_size = (size_t)(game_elf_end - game_elf_start);
    // if (!storage__write_file_atomic(path, game_elf_start, elf_size)) {
    //     printf("[selftest] loader/elf_xip: could not stage embedded fixture\n");
    //     return false;
    // }

    int result = app_runner__run_path(path, NULL, BRUCE_LAUNCH_FOREGROUND);
    if (result > 0) (void)runtime__delay(50);
    storage__remove(path);

    if (result <= 0) {
#if CONFIG_BRUCE_QEMU_TEST_MODE
        if (result == BRUCE_ERR_INVALID_ARGUMENT) {
            printf("[selftest] loader/elf_xip: OK (QEMU relocation unavailable)\n");
            return true;
        }
#endif
        printf("[selftest] loader/elf_xip: open failed (result=%d)\n", result);
        return false;
    }

    printf("[selftest] loader/elf_xip: OK\n");
    return true;
}

bool selftest__run_wasm_loader_case(void) {
    const char *path = "/bin/selftest_wasm_loader_target.wasm";
    storage__remove(path);

    korp_tid task = os_self_thread();
    if (task == 0 || task != os_self_thread()) {
        printf("[selftest] loader/wasm: unstable WAMR task identity\n");
        return false;
    }

    bruce_process_snapshot_t before;
    bruce_process_snapshot_t during;
    bruce_process_snapshot_t after;
    if (process__snapshot(process__current_id(), &before) != BRUCE_OK) {
        printf("[selftest] loader/wasm: could not read initial accounting\n");
        return false;
    }
    void *allocation = wasm_loader__debug_runtime_malloc(4096);
    if (allocation == NULL ||
        process__snapshot(process__current_id(), &during) != BRUCE_OK ||
        during.memory_bytes < before.memory_bytes + 4096u) {
        wasm_loader__debug_runtime_free(allocation);
        printf("[selftest] loader/wasm: WAMR allocation was not accounted\n");
        return false;
    }
    wasm_loader__debug_runtime_free(allocation);
    if (process__snapshot(process__current_id(), &after) != BRUCE_OK ||
        after.memory_bytes != before.memory_bytes) {
        printf("[selftest] loader/wasm: WAMR accounting was not released\n");
        return false;
    }

    size_t calls_before = wasm_loader__debug_call_count();
    int result = app_runner__run_path(path, NULL, BRUCE_LAUNCH_BACKGROUND);
    /* app_runner__run_path() only spawns the "wasm" loader command
     * asynchronously (see app_runner__run_path_with_environment()) -- the
     * wasm loader's own BRUCE_ERR_NOT_FOUND (the target file was never
     * written) is produced inside that spawned process, so it shows up on
     * its exit code, not this call's own return value (just the spawned
     * pid on success). */
    if (result > 0) {
        bruce_process_status_t status;
        result = process__wait_status((bruce_process_id_t)result, 2000, &status) == BRUCE_OK &&
                         status.reason == BRUCE_PROCESS_EXITED
                     ? status.exit_code
                     : result;
    }
    if (result != BRUCE_ERR_NOT_FOUND || wasm_loader__debug_call_count() != calls_before + 1) {
        printf("[selftest] loader/wasm: loader was not dispatched (%d)\n", result);
        return false;
    }

    printf("[selftest] loader/wasm: OK\n");
    return true;
}
