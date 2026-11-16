#pragma once

/*
 * Small bounded cache of fetched inline images, keyed by URL. Images are loaded on demand, not automatically: browser_render.c
 * only ever calls browser_image_cache__peek() while drawing, which never
 * touches the network -- a page with several images scrolls just as fast as
 * one with none. browser_app.c calls browser_image_cache__get() when the user
 * highlights an image and presses Select, which starts a background fetch;
 * browser_image_cache__poll() reports when it lands. After that, the cache decodes and box-fits it once into a memory__external-backed RGB565 bitmap. Redraws borrow that fitted bitmap and perform no
 * image decoding, scaling, or allocation. The slot also keeps the original
 * fetched bytes around (see browser_image_cache__save()/__raw()) so a loaded
 * image can be written out to storage, or put on the clipboard, as the real
 * file it is, not the decoded pixel data.
 */

#include <stdbool.h>
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

/* Fits an already-fetched `url` into an external-backed bitmap for the
 * requested box. Not fetched yet: starts a background fetch (cancelling one
 * for a different image) and returns BRUCE_ERR_BUSY; call again once
 * browser_image_cache__poll() reports it landed. The borrowed result
 * remains valid until a later get()/peek() evicts its LRU slot or the cache
 * is destroyed. Fetch and decode failures are cached to avoid repeated work. */
bruce_result_t browser_image_cache__get(
    browser_image_cache_t *cache, const char *url, uint16_t box_width, uint16_t box_height,
    bruce_display_color_t background, const image_bitmap_t **out_bitmap
);

/* Collects the background fetch. Returns true once it has finished, either
 * way, with `*out_url` set to the image's URL -- get() for it then returns the
 * image or the failure. */
bool browser_image_cache__poll(browser_image_cache_t *cache, const char **out_url);

/* True while a background image fetch is running. */
bool browser_image_cache__loading(const browser_image_cache_t *cache);

/* Borrows an already-fitted bitmap without fetching, decoding, scaling, or
 * allocating. Returns BRUCE_ERR_NOT_FOUND until get() succeeds. */
bruce_result_t browser_image_cache__peek(
    browser_image_cache_t *cache, const char *url, const image_bitmap_t **out_bitmap
);

/* Writes the exact bytes fetched for `url` -- the original encoded
 * JPEG/PNG/GIF, not the decoded-and-box-fitted bitmap peek()/get() return --
 * to `dest_path`, creating or truncating it. Returns BRUCE_ERR_NOT_FOUND if
 * `url` was never successfully fetched (get() not yet called for it, or its
 * fetch failed). */
bruce_result_t browser_image_cache__save(browser_image_cache_t *cache, const char *url, const char *dest_path);

/* Borrows the exact bytes fetched for `url` -- the same ones save() writes
 * to storage -- without touching storage at all, e.g. to put them directly
 * on the clipboard (see clipboard__set_binary()). Returns BRUCE_ERR_NOT_FOUND
 * under the same conditions as save(). The borrowed pointer remains valid
 * under the same rules as get()/peek()'s bitmap. */
bruce_result_t browser_image_cache__raw(
    browser_image_cache_t *cache, const char *url, const void **out_data, size_t *out_len
);
