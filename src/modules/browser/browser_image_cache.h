#pragma once

/*
 * Small bounded cache of fetched inline images, keyed by URL. Images are loaded on demand, not automatically: browser_render.c
 * only ever calls browser_image_cache__peek() while drawing, which never
 * touches the network -- a page with several images scrolls just as fast as
 * one with none. browser_app.c calls browser_image_cache__get() exactly
 * once, when the user highlights an image and presses Select, to actually
 * fetch it; after that, the cache decodes and box-fits it once into a memory__external-backed RGB565 bitmap. Redraws borrow that fitted bitmap and perform no
 * image decoding, scaling, or allocation.
 */

#include <stddef.h>

#include "core_sdk/image.h"
#include "core_sdk/result.h"

#define BROWSER_IMAGE_CACHE_SLOTS 4
#define BROWSER_IMAGE_MAX_BYTES (256u * 1024u)

/* How long a failed fetch is remembered before it's worth retrying. Without
 * this, an image that fails once (a transient network hiccup, or the page
 * simply scrolled back into view) gets re-fetched -- and re-fails -- on
 * every single redraw, hammering the network and spamming the log for
 * nothing. */
#define BROWSER_IMAGE_FAIL_COOLDOWN_MS 15000u

typedef struct browser_image_cache browser_image_cache_t;

bruce_result_t browser_image_cache__create(browser_image_cache_t **out_cache);
void browser_image_cache__destroy(browser_image_cache_t *cache);

/* Fetches and decodes `url` on first use, then fits it into an external-
 * backed bitmap for the requested box. The borrowed result remains valid
 * until a later get()/peek() evicts its LRU slot or the cache is destroyed.
 * Fetch and decode failures are cached to avoid repeated work. */
bruce_result_t browser_image_cache__get(
    browser_image_cache_t *cache, const char *url, uint16_t box_width, uint16_t box_height,
    bruce_display_color_t background, const image_bitmap_t **out_bitmap
);

/* Borrows an already-fitted bitmap without fetching, decoding, scaling, or
 * allocating. Returns BRUCE_ERR_NOT_FOUND until get() succeeds. */
bruce_result_t browser_image_cache__peek(
    browser_image_cache_t *cache, const char *url, const image_bitmap_t **out_bitmap
);
