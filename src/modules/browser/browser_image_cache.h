#pragma once

/*
 * Small bounded cache of fetched (still-compressed) inline image bytes, keyed
 * by URL. Images are loaded on demand, not automatically: browser_render.c
 * only ever calls browser_image_cache__peek() while drawing, which never
 * touches the network -- a page with several images scrolls just as fast as
 * one with none. browser_app.c calls browser_image_cache__get() exactly
 * once, when the user highlights an image and presses Select, to actually
 * fetch it; after that, redraws pick it up from the cache via peek(). The
 * image itself is still decoded and box-fitted fresh on every draw by
 * browser_render.c -- decoding an already-small compressed buffer is cheap
 * compared to a round trip over HTTP.
 */

#include <stddef.h>

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

/* Returns still-encoded (JPEG/PNG/GIF) bytes for `url`, fetching over HTTP on
 * a cache miss and evicting the least-recently-used slot if the cache is
 * full. The returned pointer/length are borrowed: valid until the next
 * browser_image_cache__get()/peek() call on this cache (which may evict the
 * slot backing them) or until the cache is destroyed. Requires the calling
 * process to hold `http`. Only call this in response to an explicit user
 * action (see the file comment above) -- for drawing, use peek() below.
 *
 * A failed fetch is cached too (see BROWSER_IMAGE_FAIL_COOLDOWN_MS): calling
 * this again for the same URL within the cooldown window returns the
 * original failure immediately, with no network activity at all. */
bruce_result_t
browser_image_cache__get(browser_image_cache_t *cache, const char *url, const void **out_data, size_t *out_len);

/* Like get(), but never touches the network: returns BRUCE_ERR_NOT_FOUND if
 * `url` hasn't been fetched (successfully or not) yet, instead of fetching
 * it. This is what browser_render.c calls on every draw. */
bruce_result_t
browser_image_cache__peek(browser_image_cache_t *cache, const char *url, const void **out_data, size_t *out_len);
