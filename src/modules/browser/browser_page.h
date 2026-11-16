#pragma once

/* Fetches one page over HTTP and streams it directly into a browser_document_t
 * through core_sdk/html.h -- the raw HTML body is never buffered whole in
 * memory, only the compact extracted content is kept. */

#include <stdbool.h>
#include <stddef.h>

#include "browser_document.h"
#include "core_sdk/result.h"

#define BROWSER_HOME_URL "https://bruce.computer/"

/* Adds a "http://" scheme to `raw_url` when it has none (a bare host like
 * "example.com" or "bruce.computer" is the common case typed into the URL
 * bar); an already-absolute URL is copied unchanged. Returns false if the
 * result doesn't fit `out_capacity`. */
bool browser_page__normalize_url(const char *raw_url, char *out_url, size_t out_capacity);

typedef struct browser_page_load browser_page_load_t;

/* Starts loading `url` (must already be absolute -- see
 * browser_page__normalize_url()) into `doc`, which is reset first. The
 * response streams in the background (http__request_start() with `stream`),
 * so the caller stays responsive and can stop at any time. Requires the
 * `http` permission; does not imply `wifi`. */
bruce_result_t
browser_page__load_start(const char *url, browser_document_t *doc, browser_page_load_t **out_load);

/* Parses whatever has arrived since the last call. Returns BRUCE_ERR_BUSY
 * while the page is still loading; otherwise the load is over and this
 * returns BRUCE_OK or the transport error (DNS, connect, timeout, size
 * limit). A non-2xx status is not a failure: the body (often a real HTML
 * error page) still parses, and the status is reported through
 * `*out_status_code`. `*out_received` is the body bytes parsed so far. */
bruce_result_t browser_page__load_step(browser_page_load_t *load, int *out_status_code, size_t *out_received);

/* Stops the load if it is still running and frees it. The document keeps
 * everything parsed so far. NULL is a no-op. */
void browser_page__load_end(browser_page_load_t *load);
