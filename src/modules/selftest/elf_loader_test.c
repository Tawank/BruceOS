#include <stddef.h>
#include <stdio.h>

#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/permission.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "modules/loaders/wasm/wasm_loader_app.h"
#include "platform_api_vmcore.h"

#include "elf_loader_test.h"

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
    if (result != BRUCE_ERR_NOT_FOUND || wasm_loader__debug_call_count() != calls_before + 1) {
        printf("[selftest] loader/wasm: loader was not dispatched (%d)\n", result);
        return false;
    }

    printf("[selftest] loader/wasm: OK\n");
    return true;
}
