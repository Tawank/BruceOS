#include "memory_test.h"

#include <stdio.h>
#include <string.h>

#include "core/memory/memory.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"

bool selftest__run_memory_layout_case(void) {
    void *allocation = memory__malloc(123);
    if (allocation == NULL) {
        printf("[selftest] memory/layout: allocation failed\n");
        return false;
    }
    memset(allocation, 0x5a, 123);
    uint8_t readback[16] = {0};
    bool read_ok = memory__read(
        BRUCE_MEMORY_BACKEND_INTERNAL, (uintptr_t)allocation, readback, sizeof(readback)
    ) == BRUCE_OK;
    if (!read_ok) {
        read_ok = memory__read(
            BRUCE_MEMORY_BACKEND_PSRAM, (uintptr_t)allocation, readback, sizeof(readback)
        ) == BRUCE_OK;
    }
    read_ok = read_ok && readback[0] == 0x5a && readback[sizeof(readback) - 1] == 0x5a;
    size_t readable = 0;
    bool span_ok = memory__readable_size(
                       BRUCE_MEMORY_BACKEND_INTERNAL, (uintptr_t)allocation, &readable
                   ) == BRUCE_OK;
    if (!span_ok) {
        span_ok = memory__readable_size(
                      BRUCE_MEMORY_BACKEND_PSRAM, (uintptr_t)allocation, &readable
                  ) == BRUCE_OK;
    }
    read_ok = read_ok && span_ok && readable >= 123;
    size_t internal_count = 0;
    size_t psram_count = 0;
    if (memory__get_layout(BRUCE_MEMORY_BACKEND_INTERNAL, NULL, 0, &internal_count) != BRUCE_OK ||
        memory__get_layout(BRUCE_MEMORY_BACKEND_PSRAM, NULL, 0, &psram_count) != BRUCE_OK) {
        memory__free(allocation);
        printf("[selftest] memory/layout: count failed\n");
        return false;
    }
    size_t capacity = (internal_count > psram_count ? internal_count : psram_count) + 4;
    bruce_memory_layout_block_t *blocks = memory__malloc(capacity * sizeof(*blocks));
    if (blocks == NULL) {
        memory__free(allocation);
        printf("[selftest] memory/layout: snapshot allocation failed\n");
        return false;
    }

    size_t count = 0;
    bruce_result_t result = BRUCE_OK;
    bool found = false;
    const bruce_memory_backend_t heaps[] = {
        BRUCE_MEMORY_BACKEND_INTERNAL,
        BRUCE_MEMORY_BACKEND_PSRAM,
    };
    for (size_t heap = 0; heap < sizeof(heaps) / sizeof(heaps[0]) && !found; ++heap) {
        result = memory__get_layout(heaps[heap], blocks, capacity, &count);
        if (result != BRUCE_OK) break;
        size_t shown = count < capacity ? count : capacity;
        for (size_t i = 0; i < shown; ++i) {
            uintptr_t end = blocks[i].address + blocks[i].size;
            if (blocks[i].tracked && blocks[i].region != BRUCE_MEMORY_REGION_UNKNOWN &&
                blocks[i].region_start < blocks[i].region_end &&
                blocks[i].region_start <= blocks[i].address && end <= blocks[i].region_end &&
                blocks[i].address < (uintptr_t)allocation &&
                (uintptr_t)allocation < end && blocks[i].requested_size == 123 &&
                blocks[i].owner_id == process__current_id()) {
                found = true;
                break;
            }
        }
    }
    memory__free(blocks);
    memory__free(allocation);
    bool ok = result == BRUCE_OK && found && read_ok;
    printf("[selftest] memory/layout: %s (blocks=%u)\n", ok ? "OK" : "failed", (unsigned)count);
    return ok;
}

bool selftest__run_external_memory_case(void) {
    bruce_memory_stats_t before;
    if (memory__get_stats(&before) != BRUCE_OK) {
        printf("[selftest] memory/external: statistics unavailable\n");
        return false;
    }

    bruce_memory_object_t object;
    bruce_result_t allocation = memory_external__alloc(32, false, true, &object);
    if (allocation != BRUCE_OK || object.handle == 0 ||
        object.size != 32 || object.backend == BRUCE_MEMORY_BACKEND_INVALID) {
        printf("[selftest] memory/external: allocation failed (%d)\n", allocation);
        return false;
    }

    uint8_t initial[32] = {0};
    uint8_t changed[8];
    memset(changed, 0xA5, sizeof(changed));
    const void *mapping = NULL;
    bruce_result_t initial_write = memory_external__write(&object, 0, initial, sizeof(initial));
    bruce_result_t changed_write = memory_external__write(&object, 12, changed, sizeof(changed));
    bruce_result_t mapped = memory_external__map(&object, &mapping);
    bool ok = initial_write == BRUCE_OK && changed_write == BRUCE_OK && mapped == BRUCE_OK && mapping != NULL;
#if !CONFIG_BRUCE_QEMU_TEST_MODE
    ok = ok && memcmp((const uint8_t *)mapping + 12, changed, sizeof(changed)) == 0;
#endif

    bruce_process_snapshot_t snapshot;
    ok = ok && process__snapshot(process__current_id(), &snapshot) == BRUCE_OK &&
         snapshot.memory_bytes >= object.size;

    bruce_memory_backend_t backend = object.backend;
    ok = ok && memory_external__release(&object) == BRUCE_OK && object.handle == 0;
    bruce_memory_stats_t after;
    ok = ok && memory__get_stats(&after) == BRUCE_OK;
    if (backend == BRUCE_MEMORY_BACKEND_SWAP) {
        ok = ok && after.swap_free == before.swap_free;
    }

    printf(
        "[selftest] memory/external: %s (backend=%d write=%d/%d map=%d)\n",
        ok ? "OK" : "failed",
        backend,
        initial_write,
        changed_write,
        mapped
    );
    return ok;
}

/* Regression coverage for the flash-backed (XIP) allocation path used by the
 * ELF loader: writes must be visible through the *data-bus* mapping the
 * caller reads back through, not just on the instruction-bus mapping used to
 * execute the code. esp_partition_write()'s automatic cache invalidation only
 * flushes the alias matching the physical page's tracked MMU capability
 * (instruction, for an XIP record) -- it never touches a separate data-bus
 * alias of the same page, regardless of whether that alias came from
 * spi_flash_phys2cache() or a second tracked esp_partition_mmap(). The data
 * alias must be invalidated manually after every write. */
bool selftest__run_external_memory_xip_case(void) {
    bruce_ext_mem_loader_xip_image_t image;
    bruce_result_t allocation = ext_mem_loader__allocate_xip(64, &image);
    if (allocation != BRUCE_OK || image.instruction == NULL || image.data == NULL ||
        image.size != 64) {
        printf("[selftest] memory/external_xip: allocation failed (%d)\n", allocation);
        return false;
    }

    uint8_t first_chunk[40];
    memset(first_chunk, 0x5A, sizeof(first_chunk));
    uint8_t second_chunk[24];
    memset(second_chunk, 0xC3, sizeof(second_chunk));

    bool ok = ext_mem_loader__write_xip(&image, 0, first_chunk, sizeof(first_chunk)) == BRUCE_OK &&
              ext_mem_loader__write_xip(&image, sizeof(first_chunk), second_chunk, sizeof(second_chunk)) == BRUCE_OK;

#if !CONFIG_BRUCE_QEMU_TEST_MODE
    ok = ok && memcmp(image.data, first_chunk, sizeof(first_chunk)) == 0 &&
         memcmp(image.data + sizeof(first_chunk), second_chunk, sizeof(second_chunk)) == 0;
    ok = ok && memcmp(image.instruction, first_chunk, sizeof(first_chunk)) == 0 &&
         memcmp(image.instruction + sizeof(first_chunk), second_chunk, sizeof(second_chunk)) == 0;
#endif

    ok = ext_mem_loader__release_xip(&image) == BRUCE_OK && ok;

    printf("[selftest] memory/external_xip: %s\n", ok ? "OK" : "failed");
    return ok;
}
