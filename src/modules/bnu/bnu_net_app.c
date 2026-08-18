#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/http.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Network commands: wget, curl. Both are thin wrappers around
 * http__request(); neither implies the `wifi` permission, so callers are
 * expected to already have a working Wi-Fi connection.
 */

/* Generous cap on top of BRUCE_HTTP_DEFAULT_MAX_RESPONSE_BYTES: wget/curl
 * exist to move whole files, not just small API payloads. */
#define BNU__HTTP_MAX_RESPONSE_BYTES (16u * 1024u * 1024u)

/* esp_http_client rejects a URL with no scheme outright ("Failed to set
 * URL"), unlike wget/curl which both default a bare host/IP to http://. Match
 * that convenience so e.g. `wget example.com` works the way users expect. */
#define BNU__URL_MAX 256

/* Real wget/curl identify themselves via User-Agent by default, and some
 * sites (e.g. ysap.sh) sniff that header to serve terminal-friendly output
 * instead of an HTML page. esp_http_client's own default ("ESP32 HTTP
 * Client/1.0") doesn't match either tool's UA, so those sites can't tell
 * these commands apart from a browser. Send a plausible tool UA to match,
 * overridable via -U/--user-agent (wget) or -A/--user-agent (curl). */
#define BNU__WGET_DEFAULT_USER_AGENT "Wget/1.21.3"
#define BNU__CURL_DEFAULT_USER_AGENT "curl/8.5.0"

static bool bnu__normalize_url(const char *url, char *out_url, size_t capacity) {
    if (url == NULL || url[0] == '\0') return false;
    int written = strstr(url, "://") != NULL ? snprintf(out_url, capacity, "%s", url)
                                              : snprintf(out_url, capacity, "http://%s", url);
    return written >= 0 && (size_t)written < capacity;
}

/* Derives an output file name from the last path segment of `url` (with any
 * query string stripped), falling back to "index.html" when the URL has no
 * path at all (e.g. "http://example.com") or that segment doesn't fit
 * `capacity`. The scheme is skipped first so "://" isn't mistaken for a path
 * separator and the host isn't mistaken for a file name. */
static void bnu__wget_default_name(const char *url, char *name, size_t capacity) {
    const char *scheme_sep = strstr(url, "://");
    const char *after_host = scheme_sep != NULL ? scheme_sep + 3 : url;
    const char *path_start = strchr(after_host, '/');
    const char *start = NULL;
    size_t length = 0;
    if (path_start != NULL) {
        start = strrchr(path_start, '/') + 1;
        const char *query = strchr(start, '?');
        length = query != NULL ? (size_t)(query - start) : strlen(start);
    }
    if (start == NULL || length == 0 || length >= capacity) {
        snprintf(name, capacity, "index.html");
        return;
    }
    memcpy(name, start, length);
    name[length] = '\0';
}

static bool bnu__wget_output_path(const char *url, const char *output, char *out_path) {
    if (output != NULL && output[0] != '\0') return bnu__resolve_path(output, out_path);
    char name[BRUCE_STORAGE_NAME_MAX];
    bnu__wget_default_name(url, name, sizeof(name));
    return bnu__resolve_path(name, out_path);
}

typedef struct {
    bruce_file_id_t file;
    bruce_result_t result;
} bnu__wget_sink_t;

static bruce_result_t bnu__wget_chunk(const void *data, size_t data_len, void *context) {
    bnu__wget_sink_t *sink = context;
    size_t written = 0;
    sink->result = storage__write(sink->file, data, data_len, &written);
    return sink->result;
}

int bnu_wget_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Download a file over HTTP/HTTPS.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "url", "URL to download (scheme optional, defaults to http://)");
    ap_add_str_opt(parser, "O", NULL);
    ap_set_opt_help(parser, "O", "Save to this path instead of the URL's file name");
    ap_add_str_opt(parser, "U user-agent", NULL);
    ap_set_opt_help(parser, "U user-agent", "Send this User-Agent header instead of the default");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *raw_url = ap_get_arg(parser, "url");
    const char *output = ap_get_str_value(parser, "O");
    const char *user_agent = ap_get_str_value(parser, "U");
    if (user_agent == NULL) user_agent = BNU__WGET_DEFAULT_USER_AGENT;

    char url[BNU__URL_MAX];
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__normalize_url(raw_url, url, sizeof(url)) && bnu__wget_output_path(url, output, path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    bnu__wget_sink_t sink = {.result = BRUCE_OK};
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &sink.file
    );
    if (result != BRUCE_OK) {
        stdio__printf("wget: %s: %s\n", path, app_runner__result_to_string(result));
        return result;
    }

    stdio__printf("Saving to: '%s'\n", path);
    const char *headers[] = {"User-Agent", user_agent};
    bruce_http_request_t request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = 1,
        .max_response_bytes = BNU__HTTP_MAX_RESPONSE_BYTES,
        .on_response_chunk = bnu__wget_chunk,
        .response_chunk_context = &sink,
    };
    bruce_http_response_t response = {0};
    result = http__request(&request, &response);
    storage__close(sink.file);
    if (result == BRUCE_OK && sink.result != BRUCE_OK) result = sink.result;
    if (result != BRUCE_OK) {
        stdio__printf("wget: %s: %s\n", url, app_runner__result_to_string(result));
        return result;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        stdio__printf("wget: %s: server returned HTTP %d\n", url, response.status_code);
        http__response_free(&response);
        return BRUCE_ERR_IO;
    }
    stdio__printf("%s saved [%u]\n", path, (unsigned)response.body_len);
    http__response_free(&response);
    return BRUCE_OK;
}

int bnu_curl_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Transfer data from or to a URL.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "url", "URL to request (scheme optional, defaults to http://)");
    ap_add_str_opt(parser, "X", NULL);
    ap_set_opt_help(parser, "X", "Request method (defaults to GET, or POST when -d is given)");
    ap_add_str_opt(parser, "d", NULL);
    ap_set_opt_help(parser, "d", "Send this string as the request body");
    ap_add_str_opt(parser, "o", NULL);
    ap_set_opt_help(parser, "o", "Write the response body to this path instead of stdout");
    ap_add_flag(parser, "I");
    ap_set_opt_help(parser, "I", "Fetch headers only (HTTP HEAD)");
    ap_add_flag(parser, "i");
    ap_set_opt_help(parser, "i", "Print response headers before the body");
    ap_add_str_opt(parser, "A user-agent", NULL);
    ap_set_opt_help(parser, "A user-agent", "Send this User-Agent header instead of the default");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    const char *raw_url = ap_get_arg(parser, "url");
    const char *method = ap_get_str_value(parser, "X");
    const char *data = ap_get_str_value(parser, "d");
    const char *output = ap_get_str_value(parser, "o");
    bool head_only = ap_found(parser, "I");
    bool include_headers = ap_found(parser, "i");
    const char *user_agent = ap_get_str_value(parser, "A");
    if (user_agent == NULL) user_agent = BNU__CURL_DEFAULT_USER_AGENT;

    char url[BNU__URL_MAX];
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved =
        bnu__normalize_url(raw_url, url, sizeof(url)) && (output == NULL || bnu__resolve_path(output, path));
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    if (head_only) method = "HEAD";
    else if (method == NULL && data != NULL) method = "POST";

    const char *headers[] = {"User-Agent", user_agent};
    bruce_http_request_t request = {
        .url = url,
        .method = method,
        .body = data,
        .body_len = data != NULL ? strlen(data) : 0,
        .headers = headers,
        .header_count = 1,
        .max_response_bytes = BNU__HTTP_MAX_RESPONSE_BYTES,
    };
    bruce_http_response_t response = {0};
    bruce_result_t result = http__request(&request, &response);
    if (result != BRUCE_OK) {
        stdio__printf("curl: %s: error %d\n", url, result);
        return result;
    }

    if (include_headers || head_only) {
        stdio__printf("HTTP %d\n", response.status_code);
        for (size_t i = 0; i < response.header_count; ++i) {
            stdio__printf("%s: %s\n", response.header_names[i], response.header_values[i]);
        }
        stdio__printf("\n");
    }

    if (!head_only && response.body_len > 0) {
        if (output != NULL) {
            bruce_file_id_t file;
            result = storage__open(
                path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
            );
            if (result == BRUCE_OK) {
                size_t written = 0;
                result = storage__write(file, response.body, response.body_len, &written);
                storage__close(file);
            }
            if (result != BRUCE_OK) stdio__printf("curl: %s: error %d\n", path, result);
        } else {
            result = stdio__write(response.body, response.body_len);
        }
    }

    http__response_free(&response);
    return result;
}
