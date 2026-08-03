#include "memory_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/loader.h"
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

/* Regression coverage for the flash-backed (XIP) allocation path used by the
 * ELF loader: writes must be visible through the *data-bus* mapping the
 * caller reads back through, not just on the instruction-bus mapping used to
 * execute the code. A prior bug obtained that data-bus pointer via
 * spi_flash_phys2cache() instead of a tracked esp_partition_mmap(), so the
 * flash cache was never invalidated on write and read-back always returned
 * stale (pre-write) bytes. */
bool selftest__run_external_memory_xip_case(void) {
    bruce_loader_xip_image_t image;
    if (loader__allocate_xip(64, &image) != BRUCE_OK || image.instruction == NULL || image.data == NULL ||
        image.size != 64) {
        printf("[selftest] memory/external_xip: allocation failed\n");
        return false;
    }

    uint8_t first_chunk[40];
    memset(first_chunk, 0x5A, sizeof(first_chunk));
    uint8_t second_chunk[24];
    memset(second_chunk, 0xC3, sizeof(second_chunk));

    bool ok = loader__write_xip(&image, 0, first_chunk, sizeof(first_chunk)) == BRUCE_OK &&
              loader__write_xip(&image, sizeof(first_chunk), second_chunk, sizeof(second_chunk)) == BRUCE_OK;

    ok = ok && memcmp(image.data, first_chunk, sizeof(first_chunk)) == 0 &&
         memcmp(image.data + sizeof(first_chunk), second_chunk, sizeof(second_chunk)) == 0;
    ok = ok && memcmp(image.instruction, first_chunk, sizeof(first_chunk)) == 0 &&
         memcmp(image.instruction + sizeof(first_chunk), second_chunk, sizeof(second_chunk)) == 0;

    ok = loader__release_xip(&image) == BRUCE_OK && ok;

    printf("[selftest] memory/external_xip: %s\n", ok ? "OK" : "failed");
    return ok;
}
