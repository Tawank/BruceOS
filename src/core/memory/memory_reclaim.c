#include "core_sdk/memory.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "core/process/process.h"
#include "core_sdk/process.h"

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
    size_t count;
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
    for (size_t i = 0; i < grant->count; ++i) grant->restore[i]();
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

    memory_reclaim__ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memory_reclaim__slot_t snapshot[MEMORY_RECLAIM__MAX_PROVIDERS];
    size_t count = s_provider_count;
    memcpy(snapshot, s_providers, count * sizeof(*snapshot));
    xSemaphoreGive(s_mutex);

    /* Estimate first, with no side effects: never touch a single provider
     * (e.g. drop the display into direct mode) unless the total achievable
     * would actually cover the request. */
    size_t estimated_total = 0;
    for (size_t i = 0; i < count; ++i) estimated_total += snapshot[i].provider.estimate();
    if (estimated_total < needed_bytes) return BRUCE_ERR_NO_MEMORY;

    memory_reclaim__grant_t *grant = malloc(sizeof(*grant));
    if (grant == NULL) return BRUCE_ERR_NO_MEMORY;
    grant->count = 0;

    size_t freed = 0;
    for (size_t i = 0; i < count && freed < needed_bytes; ++i) {
        size_t provider_freed = snapshot[i].provider.reclaim();
        if (provider_freed == 0) continue;
        grant->restore[grant->count++] = snapshot[i].provider.restore;
        freed += provider_freed;
    }

    if (grant->count == 0) {
        free(grant);
        return BRUCE_ERR_NO_MEMORY;
    }

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
