#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/process.h"
#include "core_sdk/result.h"

/**
 * @brief Memory allocation (RAM, PSRAM, swap).
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t internal_total;
    size_t internal_free;
    size_t internal_largest_block;
    size_t internal_minimum_free;
    size_t internal_free_blocks;
    size_t psram_total;
    size_t psram_free;
    size_t psram_largest_block;
    size_t swap_total;
    size_t swap_free;
    size_t swap_largest_block;
} bruce_memory_stats_t;

typedef enum {
    BRUCE_MEMORY_BACKEND_INVALID = 0,
    BRUCE_MEMORY_BACKEND_PSRAM,
    BRUCE_MEMORY_BACKEND_SWAP,
    BRUCE_MEMORY_BACKEND_INTERNAL,
} bruce_memory_backend_t;

typedef struct {
    uint32_t handle;
    size_t size;
    bruce_memory_backend_t backend;
} bruce_memory_object_t;

/** Physical memory kind containing a layout block. */
typedef enum {
    BRUCE_MEMORY_REGION_UNKNOWN = 0,
    BRUCE_MEMORY_REGION_DRAM,
    BRUCE_MEMORY_REGION_DIRAM,
    BRUCE_MEMORY_REGION_IRAM,
    BRUCE_MEMORY_REGION_RTC_FAST,
    BRUCE_MEMORY_REGION_PSRAM,
    BRUCE_MEMORY_REGION_SWAP,
} bruce_memory_region_t;

/** One block in a point-in-time memory layout snapshot. */
typedef struct {
    uintptr_t address;
    size_t size;              /* Allocator block size, or swap reserved size. */
    uintptr_t region_start;   /* Start of the containing allocator region. */
    uintptr_t region_end;     /* Exclusive end of the containing region. */
    size_t requested_size;    /* Bruce-requested bytes; zero when unknown/free. */
    bruce_memory_backend_t backend;
    bruce_memory_region_t region;
    bruce_process_id_t owner_id; /* Invalid for free or untracked blocks. */
    uint32_t handle;          /* External-object handle; zero for heap blocks. */
    bool used;
    bool tracked;
    bool executable;
    /* True when this tracked block is a process's own task-stack buffer
     * (see process_registry__create()'s xTaskCreateStatic() call) rather
     * than a regular heap allocation. Always false for untracked or free
     * blocks. */
    bool is_stack;
} bruce_memory_layout_block_t;

/**
 * @brief Process-owned tracked heap allocator.
 *
 * ELF apps never receive libc malloc/free (those imports are rejected by
 * the loader); JS and built-in code should also prefer this allocator so
 * memory is accounted against the owning process and released
 * automatically if the process exits or is killed without freeing it
 * first.
 *
 * memory__malloc() must be called from within a Core-managed process (i.e.
 * a process started by process_registry__create()/AppRunner); calling it
 * with no current process returns NULL, the same result as an allocation
 * failure.
 *
 * @param size Number of bytes to allocate.
 */
void *memory__malloc(size_t size);

/**
 * @brief Allocates and zero-initializes count objects of size bytes.
 *
 * Returns NULL when either argument is zero or their product overflows
 * size_t.
 *
 * @param count Number of objects to allocate.
 * @param size Size of each object in bytes.
 */
void *memory__calloc(size_t count, size_t size);

/**
 * @brief Resizes a tracked allocation while preserving its process ownership.
 *
 * The libc realloc rules apply for NULL ptr and zero size.
 *
 * @param ptr Allocation to resize, or NULL to allocate a new one.
 * @param size New size in bytes.
 */
void *memory__realloc(void *ptr, size_t size);

/**
 * @brief Frees memory obtained from memory__malloc()/memory__calloc()/memory__realloc().
 *
 * NULL is a no-op. Passing any other pointer, or one already freed, is
 * undefined behaviour, matching libc free().
 *
 * @param ptr Allocation to free, or NULL.
 */
void memory__free(void *ptr);

/**
 * @brief Allocates process-owned external memory (PSRAM, swap, or internal RAM) and returns a read-only pointer.
 *
 * PSRAM is preferred when a sufficiently large block is available;
 * otherwise complete 64 KiB pages are allocated from swap. If neither is
 * available -- no PSRAM on this board, and no "swap" partition has been
 * committed yet (see partitions.csv) -- plain internal RAM is used as a
 * last resort so callers still get a usable allocation instead of NULL.
 * The allocation is released automatically when its process exits.
 *
 * The returned pointer is read-only: swap allocations are flash-mapped and
 * not directly CPU-writable, so writes must go through
 * memory__external_memcpy()/memset(), which know how to update each backend.
 *
 * @param size Number of bytes to allocate. Returns NULL for zero.
 */
const void *memory__external_malloc(size_t size);

/**
 * @brief Allocates process-owned external memory that is guaranteed directly CPU-writable.
 *
 * Same PSRAM-or-internal-RAM allocation memory__external_malloc() may also
 * land on, but never swap: unlike memory__external_malloc(), this never
 * falls back to a flash-mapped allocation, so callers that write into the
 * returned buffer through an ordinary pointer (rather than
 * memory__external_memcpy()/memset()) never end up with one that silently
 * corrupts on write. Returns NULL if neither PSRAM nor internal RAM has
 * room for `size` -- callers needing a guaranteed allocation should fall
 * back to memory__malloc() in that case.
 *
 * @param size Number of bytes to allocate. Returns NULL for zero.
 */
const void *memory__external_malloc_writable(size_t size);

/**
 * @brief Allocates zero-initialized external memory for count objects of size bytes.
 *
 * Returns NULL when either argument is zero or their product overflows size_t.
 *
 * @param count Number of objects to allocate.
 * @param size Size of each object in bytes.
 */
const void *memory__external_calloc(size_t count, size_t size);

/**
 * @brief Copies bytes into external memory without exposing a writable flash pointer.
 *
 * Swap updates that require changing a zero bit back to one rewrite the
 * affected flash sectors. Callers must synchronize writes with readers of
 * an already shared mapping.
 *
 * @param ptr Allocation obtained from memory__external_malloc()/calloc().
 * @param offset Byte offset within the allocation to write at.
 * @param data Bytes to copy in.
 * @param size Number of bytes to copy.
 */
bruce_result_t memory__external_memcpy(const void *ptr, size_t offset, const void *data, size_t size);

/**
 * @brief Fills a range of external memory with a repeated byte value.
 *
 * Semantics otherwise match memory__external_memcpy(); prefer this over a
 * memcpy() of a manually filled buffer since a zero fill is always a direct
 * flash write (no sector erase needed).
 *
 * @param ptr Allocation obtained from memory__external_malloc()/calloc().
 * @param offset Byte offset within the allocation to fill at.
 * @param value Byte value to repeat, as an unsigned char.
 * @param size Number of bytes to fill.
 */
bruce_result_t memory__external_memset(const void *ptr, size_t offset, int value, size_t size);

/**
 * @brief Frees memory obtained from memory__external_malloc()/calloc().
 *
 * NULL is a no-op. Payload bytes are not zeroed. Passing any other pointer
 * not currently owned by the calling process, or one already freed, is
 * rejected with BRUCE_ERR_INVALID_ARGUMENT / BRUCE_ERR_PERMISSION.
 *
 * @param ptr Allocation to free, or NULL.
 */
bruce_result_t memory__external_free(const void *ptr);

/**
 * @brief Returns current heap capacity plus fragmentation/high-water diagnostics.
 *
 * `internal_free_blocks` counts currently free allocator blocks across all
 * internal-capable heap regions; it includes the regions' natural
 * boundaries.
 *
 * @param out_stats Receives the current heap statistics.
 */
bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats);

/**
 * @brief Captures allocator blocks for one memory backend without allocating.
 *
 * INTERNAL and PSRAM include both free and occupied ESP-IDF heap blocks.
 * Occupied blocks allocated through memory__malloc() carry their Bruce owner;
 * other occupied blocks are returned as untracked. SWAP returns exact free
 * runs and process-owned external allocations. If capacity is too small, the
 * prefix that fits is written and out_count still reports the required count.
 *
 * @permission process
 */
bruce_result_t memory__get_layout(
    bruce_memory_backend_t backend, bruce_memory_layout_block_t *blocks,
    size_t capacity, size_t *out_count
);

/**
 * @brief Validates or copies bytes from a diagnostic memory range.
 *
 * INTERNAL and PSRAM addresses must lie wholly inside one ESP-IDF heap block;
 * MMIO, allocator metadata gaps, and unrelated address ranges are rejected.
 * SWAP addresses are byte offsets in the swap partition and must lie wholly
 * inside one currently allocated external object (free/stale pages are never
 * exposed). This is a best-effort live snapshot: separate calls may observe
 * concurrent changes.
 * Passing NULL for buffer performs validation without copying.
 *
 * @note Built-in modules only. External ELF/JS/WASM apps are denied even if
 * they hold the process permission, and the symbol is not exported to ELF.
 */
bruce_result_t memory__read(
    bruce_memory_backend_t backend, uintptr_t address, void *buffer, size_t size
);

/**
 * @brief Returns readable bytes from an address to the end of its heap block
 * or active swap object.
 *
 * @note Built-in modules only, under the same restrictions as memory__read().
 */
bruce_result_t memory__readable_size(
    bruce_memory_backend_t backend, uintptr_t address, size_t *out_size
);

/**
 * @brief One subsystem's contribution to memory__reclaim(): how much it could
 * give back right now, and the calls to actually shrink or grow it.
 *
 * estimate() must be side-effect-free and cheap -- memory__reclaim() calls it
 * on every registered provider just to decide whether reclaiming is worth
 * disturbing anything for. reclaim() actually shrinks the subsystem and
 * returns the bytes it freed, which may be less than estimate() reported (the
 * state may have changed, or another caller reclaimed it first) or zero if
 * there was nothing left to give. restore() undoes exactly the shrink that
 * reclaim() performed; it runs automatically, best-effort, when the process
 * credited with the reclaim exits (see memory__reclaim()), so it must be safe
 * to call from any context and a no-op if there is nothing to restore.
 */
typedef struct {
    const char *name; /* For logs; not required to be unique. */
    size_t (*estimate)(void);
    size_t (*reclaim)(void);
    void (*restore)(void);
} bruce_reclaim_provider_t;

/**
 * @brief Registers a subsystem as a source memory__reclaim() can draw from.
 *
 * Providers are consulted in registration order and are never unregistered
 * -- meant for a long-lived Core subsystem (e.g. the display framebuffer) to
 * call once from its own init function, not for per-allocation use.
 *
 * @note Built-in modules only; not exported to ELF.
 */
bruce_result_t memory__register_reclaimable(const bruce_reclaim_provider_t *provider);

/**
 * @brief Handle to a memory__reclaim() performed on another process's behalf,
 * to be handed to memory__reclaim_adopt() by that process.
 */
typedef struct {
    bruce_process_id_t owner_id;
    bruce_resource_id_t resource_id;
} bruce_memory_reclaim_token_t;

/**
 * @brief Tries to free at least `needed_bytes` from registered subsystems before a big allocation.
 *
 * Sums every registered provider's estimate() first and only calls reclaim()
 * on any of them if the total would actually meet `needed_bytes` -- a
 * request that can't be satisfied never disturbs anything (e.g. never
 * changes the display's rendering mode) for no benefit. Reclaiming stops as
 * soon as enough has been freed, so it never shrinks more subsystems than it
 * has to.
 *
 * On success, what was reclaimed is restored automatically when the calling
 * process exits -- the same crash-safety memory__malloc() gives ordinary
 * allocations. A caller reclaiming on behalf of a *different* process (e.g.
 * a loader, before spawning it) should pass out_token and have that process
 * call memory__reclaim_adopt() with it, so the restore is tied to the
 * process that actually needs the memory instead of to the caller.
 *
 * @param needed_bytes Bytes the caller is about to need.
 * @param out_freed Bytes actually freed; 0 if nothing was reclaimed (out_token is also cleared). May be NULL.
 * @param out_token Set to a token for memory__reclaim_adopt(), or the zero token if nothing was reclaimed. May be NULL.
 */
bruce_result_t memory__reclaim(
    size_t needed_bytes, size_t *out_freed, bruce_memory_reclaim_token_t *out_token
);

/**
 * @brief Transfers a memory__reclaim() restore obligation to the calling process.
 *
 * Call once from the process meant to benefit from a reclaim performed by a
 * different process -- mirrors ext_mem_loader__adopt_xip()'s handoff
 * pattern. The zero token (or one already adopted) is a no-op.
 *
 * @note Built-in modules only; not exported to ELF.
 *
 * @param token Token returned by memory__reclaim().
 */
bruce_result_t memory__reclaim_adopt(bruce_memory_reclaim_token_t token);

#ifdef __cplusplus
}
#endif
