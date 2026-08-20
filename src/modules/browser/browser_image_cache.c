#include "browser_image_cache.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "browser_document.h"
#include "core_sdk/http.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"

/* A failed fetch is retried once, after a short delay, before it's cached as
 * a failure. On a board with no PSRAM, an HTTPS image fetch competes with
 * everything else for internal RAM, and the specific failure this guards
 * against -- mbedTLS's per-request TLS buffer needing a contiguous ~16 KiB
 * block that internal-heap fragmentation doesn't currently have, even though
 * plenty of free bytes exist in smaller pieces -- is usually transient: the
 * fragmentation picture shifts constantly as other things allocate and free.
 * One retry a moment later costs little and recovers the common case. */
#define BROWSER_IMAGE_RETRY_DELAY_MS 150u

typedef struct {
    char url[BROWSER_URL_MAX];
    bool valid;
    bool failed;         /* Slot remembers a failed fetch rather than data. */
    uint64_t retry_at_ms; /* failed slots only: don't retry before this. */
    bruce_result_t failure;
    bruce_http_response_t response;
    image_bitmap_t bitmap;
    uint16_t bitmap_box_width;
    uint16_t bitmap_box_height;
    bruce_display_color_t bitmap_background;
    bruce_result_t bitmap_failure;
    bool bitmap_failed;
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

static void browser_image_cache__release_slot(browser_image_cache_slot_t *slot) {
    if (slot == NULL) return;
    http__response_free(&slot->response);
    image__bitmap_release(&slot->bitmap);
}

void browser_image_cache__destroy(browser_image_cache_t *cache) {
    if (cache == NULL) return;
    for (size_t i = 0; i < BROWSER_IMAGE_CACHE_SLOTS; ++i) {
        browser_image_cache__release_slot(&cache->slots[i]);
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

/* One fetch attempt: BRUCE_OK plus a filled `*response` on success (2xx with
 * a body), otherwise a negative BRUCE_ERR_* and `*response` untouched. */
static bruce_result_t browser_image_cache__fetch_once(const char *url, bruce_http_response_t *response) {
    bruce_http_request_t request = {
        .url = url,
        .method = "GET",
        .max_response_bytes = BROWSER_IMAGE_MAX_BYTES,
    };
    bruce_http_response_t attempt = {0};
    bruce_result_t result = http__request(&request, &attempt);
    if (result == BRUCE_OK && (attempt.status_code < 200 || attempt.status_code >= 300 || attempt.body == NULL)) {
        http__response_free(&attempt);
        result = BRUCE_ERR_IO;
    }
    if (result == BRUCE_OK) *response = attempt;
    return result;
}

static bruce_result_t browser_image_cache__get_encoded(browser_image_cache_t *cache, const char *url, const void **out_data, size_t *out_len) {
    if (cache == NULL || url == NULL || url[0] == '\0' || out_data == NULL || out_len == NULL)
        return BRUCE_ERR_INVALID_ARGUMENT;

    browser_image_cache_slot_t *slot = browser_image_cache__find(cache, url);
    if (slot != NULL && !slot->failed) {
        slot->last_used = ++cache->clock;
        *out_data = slot->response.body;
        *out_len = slot->response.body_len;
        return BRUCE_OK;
    }
    if (slot != NULL && slot->failed && runtime__now() < slot->retry_at_ms) {
        /* Still within the cooldown from the last attempt -- report the
         * same failure again without touching the network at all. */
        return slot->failure;
    }

    bruce_http_response_t response = {0};
    bruce_result_t result = browser_image_cache__fetch_once(url, &response);
    if (result != BRUCE_OK) {
        /* Most failures here are a transient internal-RAM squeeze (see the
         * comment on BROWSER_IMAGE_RETRY_DELAY_MS above), not a permanently
         * bad URL, so it's worth one retry before giving up on it. */
        (void)runtime__delay(BROWSER_IMAGE_RETRY_DELAY_MS);
        result = browser_image_cache__fetch_once(url, &response);
    }

    /* Reuse the slot we already found (an expired failure record for this
     * same URL) if there was one, so a URL that keeps failing doesn't churn
     * through every other slot's cache each time its cooldown lapses. */
    if (slot == NULL) slot = browser_image_cache__slot_to_reuse(cache);
    if (slot->valid) browser_image_cache__release_slot(slot);
    snprintf(slot->url, sizeof(slot->url), "%s", url);
    slot->last_used = ++cache->clock;

    if (result != BRUCE_OK) {
        slot->valid = true;
        slot->failed = true;
        slot->failure = result;
        slot->retry_at_ms = runtime__now() + BROWSER_IMAGE_FAIL_COOLDOWN_MS;
        slot->response = (bruce_http_response_t){0};
        return result;
    }

    slot->response = response;
    slot->valid = true;
    slot->failed = false;
    slot->bitmap_failed = false;

    *out_data = slot->response.body;
    *out_len = slot->response.body_len;
    return BRUCE_OK;
}

bruce_result_t browser_image_cache__get(
    browser_image_cache_t *cache, const char *url, uint16_t box_width, uint16_t box_height,
    bruce_display_color_t background, const image_bitmap_t **out_bitmap
) {
    if (out_bitmap == NULL || box_width == 0 || box_height == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_bitmap = NULL;

    const void *data = NULL;
    size_t len = 0;
    bruce_result_t result = browser_image_cache__get_encoded(cache, url, &data, &len);
    if (result != BRUCE_OK) return result;
    browser_image_cache_slot_t *slot = browser_image_cache__find(cache, url);
    if (slot == NULL) return BRUCE_ERR_NOT_FOUND;
    bool same_fit = slot->bitmap_box_width == box_width && slot->bitmap_box_height == box_height &&
                    slot->bitmap_background == background;
    if (same_fit && slot->bitmap.pixels != NULL) {
        *out_bitmap = &slot->bitmap;
        return BRUCE_OK;
    }
    if (same_fit && slot->bitmap_failed) return slot->bitmap_failure;

    image__bitmap_release(&slot->bitmap);
    slot->bitmap_box_width = box_width;
    slot->bitmap_box_height = box_height;
    slot->bitmap_background = background;
    slot->bitmap_failed = false;
    bruce_image_draw_options_t options = {.center = false, .fit = true, .background = background};
    image_bitmap_t decoded = {0};
    result = image__get_bitmap_from_memory(data, len, &options, &decoded);
    if (result == BRUCE_OK) {
        result = image__bitmap_resize(&decoded, box_width, box_height, &slot->bitmap);
    }
    image__bitmap_release(&decoded);
    if (result != BRUCE_OK) {
        slot->bitmap_failed = true;
        slot->bitmap_failure = result;
        return result;
    }
    *out_bitmap = &slot->bitmap;
    return BRUCE_OK;
}

bruce_result_t browser_image_cache__peek(
    browser_image_cache_t *cache, const char *url, const image_bitmap_t **out_bitmap
) {
    if (cache == NULL || url == NULL || url[0] == '\0' || out_bitmap == NULL)
        return BRUCE_ERR_INVALID_ARGUMENT;
    *out_bitmap = NULL;

    browser_image_cache_slot_t *slot = browser_image_cache__find(cache, url);
    if (slot == NULL) return BRUCE_ERR_NOT_FOUND;
    if (slot->failed) return slot->failure;
    if (slot->bitmap.pixels == NULL) {
        return slot->bitmap_failed ? slot->bitmap_failure : BRUCE_ERR_NOT_FOUND;
    }
    slot->last_used = ++cache->clock;
    *out_bitmap = &slot->bitmap;
    return BRUCE_OK;
}
