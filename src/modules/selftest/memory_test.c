#include "memory_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/process.h"

bool selftest__run_external_memory_case(void) {
    bruce_memory_stats_t before;
    if (memory__get_stats(&before) != BRUCE_OK || before.swap_total == 0) {
        printf("[selftest] memory/external: swap partition unavailable\n");
        return false;
    }

    bruce_memory_object_t object;
    if (memory__external_alloc(32, &object) != BRUCE_OK || object.handle == 0 ||
        object.size != 32 || object.backend == BRUCE_MEMORY_BACKEND_INVALID) {
        printf("[selftest] memory/external: allocation failed\n");
        return false;
    }

    uint8_t initial[32] = {0};
    uint8_t changed[8];
    memset(changed, 0xA5, sizeof(changed));
    const void *mapping = NULL;
    bool ok = memory__external_write(&object, 0, initial, sizeof(initial)) == BRUCE_OK &&
              memory__external_write(&object, 12, changed, sizeof(changed)) == BRUCE_OK &&
              memory__external_map(&object, &mapping) == BRUCE_OK && mapping != NULL &&
              memcmp((const uint8_t *)mapping + 12, changed, sizeof(changed)) == 0;

    bruce_process_snapshot_t snapshot;
    ok = ok && process__snapshot(process__current_id(), &snapshot) == BRUCE_OK &&
         snapshot.memory_bytes >= object.size;

    bruce_memory_backend_t backend = object.backend;
    ok = ok && memory__external_free(&object) == BRUCE_OK && object.handle == 0;
    bruce_memory_stats_t after;
    ok = ok && memory__get_stats(&after) == BRUCE_OK;
    if (backend == BRUCE_MEMORY_BACKEND_SWAP) {
        ok = ok && after.swap_free == before.swap_free;
    }

    printf("[selftest] memory/external: %s\n", ok ? "OK" : "failed");
    return ok;
}
