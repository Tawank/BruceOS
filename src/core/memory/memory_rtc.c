#include "core/memory/memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * Small first-fit, coalescing suballocator over a static RTC_DATA_ATTR
 * buffer, so memory__malloc() can give a handful of small, short-lived
 * allocations a shot at RTC memory before falling back to the general
 * internal heap (see memory__malloc()'s comment). RTC_DATA_ATTR places this
 * buffer wherever this target's ".rtc.data" section lands - RTC_SLOW memory
 * by default on ESP32-S3 (CONFIG_ESP32S3_RTCDATA_IN_FAST_MEM is unset in
 * this project's sdkconfig), which esp_heap_caps' own priority table for
 * this SoC (components/heap/port/esp32s3/memory_layout.c) never reaches for
 * a plain malloc()/MALLOC_CAP_DEFAULT request regardless - so nothing here
 * competes with the general heap for the same bytes, and there is nothing
 * in this codebase that uses RTC_SLOW memory otherwise (the ULP coprocessor,
 * the only other consumer of it on this chip, is disabled - see
 * CONFIG_ULP_COPROC_ENABLED).
 *
 * Deliberately hand-rolled rather than registered with esp_heap_caps:
 * heap_caps_add_region()/heap_caps_add_region_with_caps() only accept memory
 * that already falls inside a *known* entry of the target's static
 * soc_memory_regions[] table, and RTC_SLOW memory has no such entry for this
 * chip (unlike RTC_FAST, which does, gated behind
 * CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP) - so there is no supported
 * way to fold it into esp_heap_caps' own allocator at all.
 *
 * Not preserved across a deep-sleep wake: s_initialized lives in ordinary
 * (non-retained) memory, so memory_rtc__init_locked() always resets the pool
 * to one big free chunk on its first call after any boot, cold or
 * wake-from-sleep. This project never enters deep sleep today, but that
 * means nothing allocated here should ever be assumed to survive one.
 *
 * Sized below the segment's full 8192-byte length (rtc_slow_seg in
 * memory.ld), not equal to it: the rest of the system already places a
 * handful of bytes of its own in .rtc.data (RTC_DATA_ATTR/RTC_NOINIT_ATTR
 * users elsewhere in the tree/SDK), and claiming the full length leaves no
 * room for that, which fails at link time (confirmed: "region `rtc_slow_seg'
 * overflowed by 36 bytes" at exactly 8192u). 8000u leaves room for ESP-IDF
 * RTC sleep code and timer state, including a 32-byte margin with v6.0.2.
 */
#define MEMORY_RTC__POOL_SIZE 8000u
/* Minimum leftover worth splitting off into its own free chunk when an
 * allocation only partly fills the chunk it landed in - below this, the
 * request just takes the whole chunk instead of leaving a sliver too small
 * to ever satisfy a later allocation (its header alone would eat most of
 * it). */
#define MEMORY_RTC__MIN_SPLIT 16u

typedef struct {
    uint32_t size; /* usable bytes following this header */
    uint32_t used;
} memory_rtc__chunk_t;

/* _Alignas(4): a plain uint8_t[] only guarantees 1-byte alignment from the
 * compiler's point of view, but memory_rtc__chunk_t (and whatever
 * memory__header_t the caller places right after it) need 4-byte-aligned
 * access. */
RTC_DATA_ATTR static _Alignas(4) uint8_t s_rtc_pool[MEMORY_RTC__POOL_SIZE];

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;

static void memory_rtc__ensure_mutex(void) {
    if (s_mutex != NULL) return;
    portENTER_CRITICAL(&s_init_mux);
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    portEXIT_CRITICAL(&s_init_mux);
}

/* Caller holds s_mutex. */
static void memory_rtc__init_locked(void) {
    if (s_initialized) return;
    memory_rtc__chunk_t *first = (memory_rtc__chunk_t *)s_rtc_pool;
    first->size = (uint32_t)(sizeof(s_rtc_pool) - sizeof(memory_rtc__chunk_t));
    first->used = 0;
    s_initialized = true;
}

/* Caller holds s_mutex. Merges every run of adjacent free chunks in one
 * left-to-right pass over the whole pool - simple rather than clever, since
 * the pool is only MEMORY_RTC__POOL_SIZE bytes and this only runs on
 * memory_rtc__free(), never on the memory__malloc() hot path. */
static void memory_rtc__coalesce_locked(void) {
    uint8_t *cursor = s_rtc_pool;
    uint8_t *end = s_rtc_pool + sizeof(s_rtc_pool);
    while (cursor < end) {
        memory_rtc__chunk_t *chunk = (memory_rtc__chunk_t *)cursor;
        uint8_t *next_cursor = cursor + sizeof(memory_rtc__chunk_t) + chunk->size;
        if (!chunk->used) {
            while (next_cursor < end) {
                memory_rtc__chunk_t *next = (memory_rtc__chunk_t *)next_cursor;
                if (next->used) break;
                chunk->size += (uint32_t)(sizeof(memory_rtc__chunk_t) + next->size);
                next_cursor = cursor + sizeof(memory_rtc__chunk_t) + chunk->size;
            }
        }
        cursor = next_cursor;
    }
}

void *memory_rtc__alloc(size_t size) {
    if (size == 0 || size > SIZE_MAX - sizeof(memory_rtc__chunk_t)) return NULL;
    memory_rtc__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_rtc__init_locked();

    uint8_t *cursor = s_rtc_pool;
    uint8_t *end = s_rtc_pool + sizeof(s_rtc_pool);
    void *result = NULL;
    while (cursor < end) {
        memory_rtc__chunk_t *chunk = (memory_rtc__chunk_t *)cursor;
        if (!chunk->used && chunk->size >= size) {
            uint32_t remaining = chunk->size - (uint32_t)size;
            if (remaining >= sizeof(memory_rtc__chunk_t) + MEMORY_RTC__MIN_SPLIT) {
                memory_rtc__chunk_t *split =
                    (memory_rtc__chunk_t *)(cursor + sizeof(memory_rtc__chunk_t) + size);
                split->size = remaining - (uint32_t)sizeof(memory_rtc__chunk_t);
                split->used = 0;
                chunk->size = (uint32_t)size;
            }
            chunk->used = 1;
            result = cursor + sizeof(memory_rtc__chunk_t);
            break;
        }
        cursor += sizeof(memory_rtc__chunk_t) + chunk->size;
    }

    xSemaphoreGive(s_mutex);
    return result;
}

void memory_rtc__free(void *ptr) {
    if (ptr == NULL) return;
    memory_rtc__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_rtc__chunk_t *chunk = ((memory_rtc__chunk_t *)ptr) - 1;
    chunk->used = 0;
    memory_rtc__coalesce_locked();
    xSemaphoreGive(s_mutex);
}

void memory_rtc__walk(
    bool (*visit)(uintptr_t pool_start, uintptr_t pool_end, const memory_rtc__block_t *block, void *context),
    void *context
) {
    if (visit == NULL) return;
    memory_rtc__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_rtc__init_locked();

    uintptr_t pool_start = (uintptr_t)s_rtc_pool;
    uintptr_t pool_end = pool_start + sizeof(s_rtc_pool);
    uint8_t *cursor = s_rtc_pool;
    uint8_t *end = s_rtc_pool + sizeof(s_rtc_pool);
    while (cursor < end) {
        memory_rtc__chunk_t *chunk = (memory_rtc__chunk_t *)cursor;
        memory_rtc__block_t block = {
            .address = (uintptr_t)(cursor + sizeof(memory_rtc__chunk_t)),
            .size = chunk->size,
            .used = chunk->used,
        };
        if (!visit(pool_start, pool_end, &block, context)) break;
        cursor += sizeof(memory_rtc__chunk_t) + chunk->size;
    }

    xSemaphoreGive(s_mutex);
}
