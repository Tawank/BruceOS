#pragma once

/* Shared state between webui_app.c/webui_http.c/webui_memory.c/webui_auth.c/
 * webui_files.c/webui_system.c. Not part of the public core_sdk/ API: other
 * modules must not include this header, only webui_app.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/http_server.h"
#include "core_sdk/result.h"

/* Local per-app config (see core_sdk/app_config.h); persisted at
 * /config/webui.conf, separate from the shared /config/bruce.conf. Shared by
 * webui_auth.c (sessions, login credentials) and webui_system.c (wifi
 * credential update). */
#define WEBUI_CONFIG_APP_NAME "webui"

/* Chunk size for every streaming copy in the module: file download/upload,
 * editor content ingestion. */
#define WEBUI_IO_CHUNK 2048u

typedef enum {
    WEBUI_APP_NETWORK_EXISTING = 0,
    WEBUI_APP_NETWORK_AP,
} webui_app_network_mode_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *type;
} webui_asset_t;

/* --- webui_app.c --- */
webui_app_network_mode_t webui__get_network_mode(void);

/* --- webui_http.c ---
 * Generic HTTP reply helpers, form/query parsing, and storage-path
 * sanitization. None of these touch auth, files, or the network mode. */
bruce_result_t webui__reply(
    bruce_http_server_request_t *request, int status, const char *type, const void *body, size_t body_len
);
bruce_result_t webui__reply_text(bruce_http_server_request_t *request, int status, const char *body);
bruce_result_t webui__reply_error(bruce_http_server_request_t *request, bruce_result_t error);
bruce_result_t webui__serve_asset(bruce_http_server_request_t *request, const webui_asset_t *asset);
bruce_result_t webui__redirect(bruce_http_server_request_t *request, const char *location);

int webui__hex(char value);
/* Percent/plus-decodes source[0..length) into output, rejecting NUL/control
 * bytes (tab/lf/cr excepted) and truncation. output and source may not
 * overlap other than output preceding source in the same buffer (the way
 * webui__form_value() reuses its own form buffer for the decoded value). */
bool webui__decode(const char *source, size_t length, char *output, size_t capacity);
/* Locates the still-encoded value for `key` in a '&'-joined, NUL-terminated
 * form/query string, without decoding it. webui_files.c's webui__edit() uses
 * this directly, then hands the span to webui_memory.c's
 * webui__decode_to_object() to decode a large value (the editor's "content"
 * field) straight into external memory instead of a second in-process copy;
 * webui__form_value() below is just this plus a decode. */
bool webui__form_span(const char *form, const char *key, const char **out_start, size_t *out_length);
bool webui__form_value(const char *form, const char *key, char *output, size_t capacity);

char *webui__read_query(bruce_http_server_request_t *request);
char *webui__read_body(bruce_http_server_request_t *request, size_t maximum);

bool webui__storage_path(
    const char *fs, const char *folder, const char *name, char *output, size_t capacity
);
void webui__human_size(size_t bytes, char *output, size_t capacity);

/* --- webui_memory.c ---
 * memory__external_malloc()-backed helpers so large/variable-size buffers
 * come from PSRAM or swap when available instead of a fixed-size internal
 * allocation. */

/* Largest single allocation realistically available right now, across
 * internal RAM, PSRAM, and swap, minus a small safety margin. Replaces a
 * fixed cap (there is no more WEBUI_FORM_MAX) for buffers that go through
 * memory__external_memcpy() rather than a raw pointer, so swap is a genuine
 * option. */
size_t webui__memory_cap(void);

/* Allocates a directly-writable buffer: PSRAM or internal RAM (via
 * memory__external_malloc_writable(), tracked and released with the
 * request's process) when either has room for `size`, otherwise a plain
 * memory__malloc(). Never swap: callers write into the buffer through a raw
 * pointer (storage__list()/display__snapshot() filling it directly), which
 * swap's flash-backed allocations don't support -- only
 * memory__external_memcpy() may touch swap memory. */
bruce_result_t webui__alloc_direct(void **out_data, bool *out_external, size_t size);
void webui__free_direct(void *data, bool external);

/* Fills data[0..length) from the request body in WEBUI_IO_CHUNK pieces, via
 * memory__external_memcpy() so this works regardless of which backend
 * memory__external_malloc() picked (a straight recv() into that memory
 * would need a raw pointer, which swap can't offer). */
bruce_result_t
webui__receive_into_object(bruce_http_server_request_t *request, const void *data, size_t length);

/* Percent/plus-decodes source[0..length) into data starting at offset 0, in
 * WEBUI_IO_CHUNK-sized memory__external_memcpy() calls; *out_length receives
 * the decoded length (always <= length, so a `length`-byte allocation always
 * has room). Same validation rules as webui__decode(). */
bruce_result_t webui__decode_to_object(
    const char *source, size_t length, const void *data, size_t *out_length
);

/* --- webui_auth.c --- */
bool webui__authenticated(bruce_http_server_request_t *request);
bool webui__require_auth(bruce_http_server_request_t *request, bruce_result_t *response_result);
bruce_result_t webui__login(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__logout(bruce_http_server_request_t *request, void *context);

/* --- webui_files.c --- */
bruce_result_t webui__list_files(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__file(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__rename(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__edit(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__upload(bruce_http_server_request_t *request, void *context);

/* --- webui_system.c --- */
bruce_result_t webui__theme(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__system_info(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__command(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__wifi(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__screen(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__reboot(bruce_http_server_request_t *request, void *context);
bruce_result_t webui__api_status(bruce_http_server_request_t *request, void *context);
