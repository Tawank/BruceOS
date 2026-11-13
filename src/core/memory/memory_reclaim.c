#include "core_sdk/memory.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "core/process/process.h"
#include "core_sdk/process.h"

#define TAG "memory_reclaim"

#define MEMORY_RECLAIM__MAX_PROVIDERS 8u

typedef struct {
    bruce_reclaim_provider_t provider;
    bool in_use;
} memory_reclaim__slot_t;

/* One memory__reclaim() call may shrink several providers to reach
 * needed_bytes; this is the context a single process-owned resource carries
 * so its cleanup (memory_reclaim__restore()) can undo all of them at once
 * when the credited process exits. Heap-allocated with plain malloc/free,
 * same as process__resource_t itself (core/process/process_internal.h) --
 * this is Core bookkeeping, not a tracked app allocation. */
typedef struct {
    void (*restore[MEMORY_RECLAIM__MAX_PROVIDERS])(void);
    const char *names[MEMORY_RECLAIM__MAX_PROVIDERS]; /* parallel to restore[], for the restore-time log */
    size_t count;
    size_t freed; /* total bytes memory__reclaim() freed to grant this; logged again on restore */
} memory_reclaim__grant_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static memory_reclaim__slot_t s_providers[MEMORY_RECLAIM__MAX_PROVIDERS];
static size_t s_provider_count;

static void memory_reclaim__ensure_mutex(void) {
    if (s_mutex != NULL) return;
    portENTER_CRITICAL(&s_init_mux);
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    portEXIT_CRITICAL(&s_init_mux);
}

bruce_result_t memory__register_reclaimable(const bruce_reclaim_provider_t *provider) {
    if (provider == NULL || provider->estimate == NULL || provider->reclaim == NULL ||
        provider->restore == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    memory_reclaim__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bruce_result_t result = BRUCE_ERR_RESOURCE_LIMIT;
    if (s_provider_count < MEMORY_RECLAIM__MAX_PROVIDERS) {
        s_providers[s_provider_count] = (memory_reclaim__slot_t){.provider = *provider, .in_use = true};
        s_provider_count++;
        result = BRUCE_OK;
    }
    xSemaphoreGive(s_mutex);
    return result;
}

/* process_registry__resource_register() cleanup: runs when the process
 * credited with the reclaim exits, best-effort, regardless of whether it
 * ever called memory__reclaim_adopt() to hand that credit to someone else
 * first (see memory__reclaim_adopt()). */
static void memory_reclaim__restore(void *context) {
    memory_reclaim__grant_t *grant = (memory_reclaim__grant_t *)context;
    for (size_t i = 0; i < grant->count; ++i) {
        grant->restore[i]();
        ESP_LOGW(TAG, "restored '%s'", grant->names[i]);
    }
    ESP_LOGW(
        TAG,
        "restored %u provider(s), giving back up to %u bytes it had reclaimed -- largest free block now %u "
        "internal (%u total free), %u psram (%u total free)",
        (unsigned)grant->count, (unsigned)grant->freed,
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );
    free(grant);
}

bruce_result_t memory__reclaim(
    size_t needed_bytes, size_t *out_freed, bruce_memory_reclaim_token_t *out_token
) {
    if (out_freed != NULL) *out_freed = 0;
    if (out_token != NULL) {
        *out_token = (bruce_memory_reclaim_token_t){
            .owner_id = BRUCE_PROCESS_ID_INVALID, .resource_id = BRUCE_RESOURCE_ID_INVALID
        };
    }
    if (needed_bytes == 0) return BRUCE_OK;

    /* Nothing to do if the largest contiguous block in either internal RAM
     * or PSRAM can already cover the request on its own: reclaiming is
     * disruptive (e.g. drops the display out of buffered mode), so it
     * should only happen when a request this size would actually have
     * nowhere to fit -- total free bytes looking tight is not that; a
     * too-small largest block, in every pool the caller could plausibly
     * mean, is. Registered providers only ever give back internal RAM, so
     * this can't help a PSRAM-only shortfall either way -- checking PSRAM
     * here is purely to avoid disturbing internal-RAM providers over a
     * request PSRAM alone already has room for. */
    size_t largest_internal_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t largest_psram_before = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largest_internal_before >= needed_bytes || largest_psram_before >= needed_bytes) return BRUCE_OK;

    ESP_LOGW(
        TAG,
        "need %u bytes but largest free block is only %u internal (%u total free), %u psram (%u total free) -- "
        "asking registered providers",
        (unsigned)needed_bytes, (unsigned)largest_internal_before,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)largest_psram_before,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );

    memory_reclaim__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_reclaim__slot_t snapshot[MEMORY_RECLAIM__MAX_PROVIDERS];
    size_t count = s_provider_count;
    memcpy(snapshot, s_providers, count * sizeof(*snapshot));
    xSemaphoreGive(s_mutex);

    /* The largest-block check above already established real pressure --
     * unlike a straight "would the total achievable cover needed_bytes"
     * gate, reclaim whatever registered providers can offer even if, summed,
     * it falls short. Partial headroom (e.g. the display's whole
     * framebuffer, still less than a generous manifest heap size) is still
     * real headroom; declining to take it would leave the largest block
     * exactly as undersized as it already was. */
    memory_reclaim__grant_t *grant = malloc(sizeof(*grant));
    if (grant == NULL) return BRUCE_ERR_NO_MEMORY;
    grant->count = 0;
    grant->freed = 0;

    size_t freed = 0;
    for (size_t i = 0; i < count && freed < needed_bytes; ++i) {
        size_t provider_freed = snapshot[i].provider.reclaim();
        if (provider_freed == 0) continue;
        ESP_LOGW(TAG, "'%s' gave back %u bytes", snapshot[i].provider.name, (unsigned)provider_freed);
        grant->restore[grant->count] = snapshot[i].provider.restore;
        grant->names[grant->count] = snapshot[i].provider.name;
        grant->count++;
        freed += provider_freed;
    }
    grant->freed = freed;

    if (grant->count == 0) {
        ESP_LOGW(TAG, "no registered provider had anything to give for %u bytes needed", (unsigned)needed_bytes);
        free(grant);
        return BRUCE_ERR_NO_MEMORY;
    }

    ESP_LOGW(
        TAG,
        "freed %u of %u bytes needed -- largest free block now %u internal (%u total free), %u psram (%u total "
        "free)",
        (unsigned)freed, (unsigned)needed_bytes, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );

    bruce_resource_id_t resource_id = process_registry__resource_register(memory_reclaim__restore, grant);
    if (resource_id == BRUCE_RESOURCE_ID_INVALID) {
        /* No process to attribute this to (or registration failed) -- undo
         * everything rather than leave a shrink with no path back. */
        memory_reclaim__restore(grant);
        return BRUCE_ERR_INVALID_STATE;
    }

    if (out_freed != NULL) *out_freed = freed;
    if (out_token != NULL) {
        *out_token =
            (bruce_memory_reclaim_token_t){.owner_id = process__current_id(), .resource_id = resource_id};
    }
    return BRUCE_OK;
}

bruce_result_t memory__reclaim_adopt(bruce_memory_reclaim_token_t token) {
    if (token.resource_id == BRUCE_RESOURCE_ID_INVALID) return BRUCE_OK;
    bruce_resource_id_t new_id = BRUCE_RESOURCE_ID_INVALID;
    return process_registry__resource_transfer(token.owner_id, token.resource_id, 0, false, &new_id);
}
