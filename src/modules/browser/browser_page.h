#pragma once

/* Fetches one page over HTTP and streams it directly into a browser_document_t
 * through core_sdk/html.h -- the raw HTML body is never buffered whole in
 * memory, only the compact extracted content is kept. */

#include <stdbool.h>
#include <stddef.h>

#include "browser_document.h"
#include "core_sdk/result.h"

#define BROWSER_HOME_URL "http://bruce.computer/"

/* Called synchronously after each response chunk has been parsed. `received`
 * is the cumulative number of response-body bytes received so far. */
typedef void (*browser_page_progress_cb_t)(size_t received, void *context);

/* Adds a "http://" scheme to `raw_url` when it has none (a bare host like
 * "example.com" or "bruce.computer" is the common case typed into the URL
 * bar); an already-absolute URL is copied unchanged. Returns false if the
 * result doesn't fit `out_capacity`. */
bool browser_page__normalize_url(const char *raw_url, char *out_url, size_t out_capacity);

/* Fetches `url` (must already be absolute -- see browser_page__normalize_url())
 * and streams its response body through the HTML parser into `doc`, which is
 * reset first. Requires the `http` permission; does not imply `wifi`.
 *
 * A non-2xx HTTP status is not itself a failure: the response body (often a
 * real HTML error page) still parses into `doc` normally, and the status is
 * reported through `*out_status_code` for the caller to show. This function
 * only fails on a transport-level problem (DNS, connect, timeout, or the
 * response exceeding the size limit) or an invalid `url`. */
bruce_result_t browser_page__fetch(
    const char *url, browser_document_t *doc, int *out_status_code, browser_page_progress_cb_t progress_cb,
    void *progress_context
);
