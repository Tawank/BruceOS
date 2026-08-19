#include "browser_image_cache.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "browser_document.h"
#include "core_sdk/http.h"
#include "core_sdk/memory.h"

typedef struct {
    char url[BROWSER_URL_MAX];
    bool valid;
    bruce_http_response_t response;
    uint32_t last_used;
} browser_image_cache_slot_t;

struct browser_image_cache {
    browser_image_cache_slot_t slots[BROWSER_IMAGE_CACHE_SLOTS];
    uint32_t clock;
};

bruce_result_t browser_image_cache__create(browser_image_cache_t **out_cache) {
    if (out_cache == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    browser_image_cache_t *cache = memory__malloc(sizeof(*cache));
    if (cache == NULL) return BRUCE_ERR_NO_MEMORY;
    memset(cache, 0, sizeof(*cache));
    *out_cache = cache;
    return BRUCE_OK;
}

void browser_image_cache__destroy(browser_image_cache_t *cache) {
    if (cache == NULL) return;
    for (size_t i = 0; i < BROWSER_IMAGE_CACHE_SLOTS; ++i) {
        if (cache->slots[i].valid) http__response_free(&cache->slots[i].response);
    }
    memory__free(cache);
}

static browser_image_cache_slot_t *browser_image_cache__find(browser_image_cache_t *cache, const char *url) {
    for (size_t i = 0; i < BROWSER_IMAGE_CACHE_SLOTS; ++i) {
        if (cache->slots[i].valid && strcmp(cache->slots[i].url, url) == 0) return &cache->slots[i];
    }
    return NULL;
}

static browser_image_cache_slot_t *browser_image_cache__slot_to_reuse(browser_image_cache_t *cache) {
    browser_image_cache_slot_t *oldest = &cache->slots[0];
    for (size_t i = 0; i < BROWSER_IMAGE_CACHE_SLOTS; ++i) {
        if (!cache->slots[i].valid) return &cache->slots[i];
        if (cache->slots[i].last_used < oldest->last_used) oldest = &cache->slots[i];
    }
    return oldest;
}

bruce_result_t
browser_image_cache__get(browser_image_cache_t *cache, const char *url, const void **out_data, size_t *out_len) {
    if (cache == NULL || url == NULL || url[0] == '\0' || out_data == NULL || out_len == NULL)
        return BRUCE_ERR_INVALID_ARGUMENT;

    browser_image_cache_slot_t *slot = browser_image_cache__find(cache, url);
    if (slot != NULL) {
        slot->last_used = ++cache->clock;
        *out_data = slot->response.body;
        *out_len = slot->response.body_len;
        return BRUCE_OK;
    }

    bruce_http_request_t request = {
        .url = url,
        .method = "GET",
        .max_response_bytes = BROWSER_IMAGE_MAX_BYTES,
    };
    bruce_http_response_t response = {0};
    bruce_result_t result = http__request(&request, &response);
    if (result != BRUCE_OK) return result;
    if (response.status_code < 200 || response.status_code >= 300 || response.body == NULL) {
        http__response_free(&response);
        return BRUCE_ERR_IO;
    }

    slot = browser_image_cache__slot_to_reuse(cache);
    if (slot->valid) http__response_free(&slot->response);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->response = response;
    slot->valid = true;
    slot->last_used = ++cache->clock;

    *out_data = slot->response.body;
    *out_len = slot->response.body_len;
    return BRUCE_OK;
}
