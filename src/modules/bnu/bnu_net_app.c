#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/http.h"
#include "core_sdk/icmp.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/*
 * Network commands: wget, curl, ping. wget/curl are thin wrappers around
 * http__request(); neither implies the `wifi` permission, so callers are
 * expected to already have a working Wi-Fi connection. ping wraps
 * icmp__ping() (core/icmp/icmp.c) and does require `wifi`, since unlike an
 * HTTP request it talks to the network stack directly rather than through an
 * already-connected higher layer.
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
        stdio__printf("wget: %s: %s\n", path, result__to_string(result));
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
        stdio__printf("wget: %s: %s\n", url, result__to_string(result));
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
        stdio__printf("curl: %s: %s\n", url, result__to_string(result));
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
            if (result != BRUCE_OK) stdio__printf("curl: %s: %s\n", path, result__to_string(result));
        } else {
            result = stdio__write(response.body, response.body_len);
        }
    }

    http__response_free(&response);
    return result;
}

#define BNU__PING_HOST_MAX 128
#define BNU__PING_DEFAULT_COUNT 4
#define BNU__PING_DEFAULT_TIMEOUT_MS 1000
#define BNU__PING_DEFAULT_INTERVAL_MS 1000
/* icmp__ping()'s own default when data_size is 0 -- kept in sync with
 * ESP_PING_DEFAULT_CONFIG() only for this command's "PING host: N data
 * bytes" banner, printed before the first reply confirms the real size. */
#define BNU__PING_DEFAULT_DATA_SIZE 64

int bnu_ping_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Send ICMP echo requests to a host.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "host", "Hostname or IPv4 address to ping");
    ap_add_int_opt(parser, "c", BNU__PING_DEFAULT_COUNT);
    ap_set_opt_help(parser, "c", "Number of requests to send (0 = until interrupted)");
    ap_add_int_opt(parser, "i", BNU__PING_DEFAULT_INTERVAL_MS);
    ap_set_opt_help(parser, "i", "Interval between requests, in milliseconds");
    ap_add_int_opt(parser, "W", BNU__PING_DEFAULT_TIMEOUT_MS);
    ap_set_opt_help(parser, "W", "Time to wait for each reply, in milliseconds");
    ap_add_int_opt(parser, "s", 0);
    ap_set_opt_help(parser, "s", "ICMP payload size in bytes (default: 64)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    char host[BNU__PING_HOST_MAX];
    int written = snprintf(host, sizeof(host), "%s", ap_get_arg(parser, "host"));
    int count = ap_get_int_value(parser, "c");
    int interval_ms = ap_get_int_value(parser, "i");
    int timeout_ms = ap_get_int_value(parser, "W");
    int data_size = ap_get_int_value(parser, "s");
    ap_free(parser);
    if (written < 0 || (size_t)written >= sizeof(host)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (data_size < 0 || (uint32_t)data_size > ICMP__MAX_DATA_SIZE) return BRUCE_ERR_INVALID_ARGUMENT;
    if (count < 0) count = BNU__PING_DEFAULT_COUNT;
    if (interval_ms <= 0) interval_ms = BNU__PING_DEFAULT_INTERVAL_MS;
    if (timeout_ms <= 0) timeout_ms = BNU__PING_DEFAULT_TIMEOUT_MS;

    stdio__printf(
        "PING %s: %u data bytes\n", host, data_size > 0 ? (unsigned)data_size : (unsigned)BNU__PING_DEFAULT_DATA_SIZE
    );
    int sent = 0;
    int received = 0;
    uint32_t min_ms = UINT32_MAX;
    uint32_t max_ms = 0;
    uint64_t sum_ms = 0;
    bruce_result_t loop_result = BRUCE_OK;
    for (int seq = 1; count == 0 || seq <= count; ++seq) {
        sent++;
        uint32_t round_trip_ms = 0;
        uint32_t reply_size = 0;
        uint8_t ttl = 0;
        bruce_result_t ping_result =
            icmp__ping(host, (uint32_t)timeout_ms, (uint32_t)data_size, &round_trip_ms, &reply_size, &ttl);
        if (ping_result == BRUCE_OK) {
            received++;
            if (round_trip_ms < min_ms) min_ms = round_trip_ms;
            if (round_trip_ms > max_ms) max_ms = round_trip_ms;
            sum_ms += round_trip_ms;
            stdio__printf(
                "%u bytes from %s: icmp_seq=%d ttl=%u time=%u ms\n", (unsigned)reply_size, host, seq, (unsigned)ttl,
                (unsigned)round_trip_ms
            );
        } else if (ping_result == BRUCE_ERR_TIMEOUT) {
            stdio__printf("Request timeout for icmp_seq=%d\n", seq);
        } else {
            stdio__printf("ping: %s: %s\n", host, result__to_string(ping_result));
            loop_result = ping_result;
            break;
        }
        bool last_attempt = count != 0 && seq == count;
        if (!last_attempt) {
            bruce_result_t delay_result = runtime__delay((uint32_t)interval_ms);
            if (delay_result != BRUCE_OK) {
                loop_result = BRUCE_ERR_CANCELLED;
                break;
            }
        }
    }

    int loss_percent = sent > 0 ? (int)(100 - (100 * received / sent)) : 0;
    stdio__printf(
        "\n--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n", host, sent,
        received, loss_percent
    );
    if (received > 0) {
        stdio__printf(
            "round-trip min/avg/max = %u/%u/%u ms\n", (unsigned)min_ms, (unsigned)(sum_ms / (uint64_t)received),
            (unsigned)max_ms
        );
    }
    if (loop_result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (loop_result != BRUCE_OK) return loop_result;
    return received > 0 ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
}
