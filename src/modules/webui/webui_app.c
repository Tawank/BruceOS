#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/http_server.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/task.h"
#include "core_sdk/wifi.h"
#include "webfiles.h"

#define WEBUI_FORM_MAX (64u * 1024u)
#define WEBUI_QUERY_MAX 1024u
#define WEBUI_UPLOAD_MAX (8u * 1024u * 1024u)
#define WEBUI_IO_CHUNK 2048u
#define WEBUI_DELETE_DEPTH_MAX 16u
#define WEBUI_DELETE_ENTRIES_MAX 256u

typedef enum {
    WEBUI_APP_NETWORK_EXISTING = 0,
    WEBUI_APP_NETWORK_AP,
} webui_app_network_mode_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    const char *type;
} webui_asset_t;

static webui_app_network_mode_t s_network_mode;

static const webui_asset_t s_index_asset = {
    webui_index_html_gz, webui_index_html_gz_size, "text/html; charset=utf-8"
};
static const webui_asset_t s_login_asset = {
    webui_login_html_gz, webui_login_html_gz_size, "text/html; charset=utf-8"
};
static const webui_asset_t s_css_asset = {
    webui_index_css_gz, webui_index_css_gz_size, "text/css; charset=utf-8"
};
static const webui_asset_t s_js_asset = {
    webui_index_js_gz, webui_index_js_gz_size, "text/javascript; charset=utf-8"
};

static bruce_result_t webui__reply(
    bruce_http_server_request_t *request, int status, const char *type, const void *body, size_t body_len
) {
    bruce_result_t result = http_server_request__set_status(request, status);
    if (result == BRUCE_OK) result = http_server_request__set_type(request, type);
    if (result == BRUCE_OK) result = http_server_request__send(request, body, body_len);
    return result;
}

static bruce_result_t webui__reply_text(bruce_http_server_request_t *request, int status, const char *body) {
    return webui__reply(request, status, "text/plain; charset=utf-8", body, strlen(body));
}

static bruce_result_t webui__reply_error(bruce_http_server_request_t *request, bruce_result_t error) {
    if (error == BRUCE_ERR_PERMISSION) return webui__reply_text(request, 403, "Forbidden");
    if (error == BRUCE_ERR_NOT_FOUND) return webui__reply_text(request, 404, "Not found");
    if (error == BRUCE_ERR_INVALID_ARGUMENT || error == BRUCE_ERR_INVALID_PATH)
        return webui__reply_text(request, 400, "Invalid request");
    if (error == BRUCE_ERR_NO_MEMORY || error == BRUCE_ERR_RESOURCE_LIMIT)
        return webui__reply_text(request, 503, "Resource unavailable");
    if (error == BRUCE_ERR_ALREADY_EXISTS) return webui__reply_text(request, 409, "Already exists");
    return webui__reply_text(request, 500, "Operation failed");
}

static bruce_result_t webui__serve_asset(bruce_http_server_request_t *request, const webui_asset_t *asset) {
    bruce_result_t result = http_server_request__set_type(request, asset->type);
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Content-Encoding", "gzip");
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Cache-Control", "no-cache");
    if (result == BRUCE_OK) result = http_server_request__send(request, asset->data, asset->size);
    return result;
}

static bool webui__cookie_token(bruce_http_server_request_t *request, char *token, size_t capacity) {
    size_t length = http_server_request__header_length(request, "Cookie");
    if (length == 0 || length > 1024u) return false;
    char *cookie = malloc(length + 1u);
    if (cookie == NULL) return false;
    bool found = false;
    if (http_server_request__get_header(request, "Cookie", cookie, length + 1u) == BRUCE_OK) {
        char *item = cookie;
        while (*item != '\0') {
            while (*item == ' ' || *item == ';') item++;
            char *end = strchr(item, ';');
            size_t item_len = end != NULL ? (size_t)(end - item) : strlen(item);
            static const char prefix[] = "BRUCESESSION=";
            if (item_len >= sizeof(prefix) - 1u && memcmp(item, prefix, sizeof(prefix) - 1u) == 0) {
                size_t token_len = item_len - (sizeof(prefix) - 1u);
                if (token_len > 0 && token_len < capacity) {
                    memcpy(token, item + sizeof(prefix) - 1u, token_len);
                    token[token_len] = '\0';
                    found = true;
                }
                break;
            }
            if (end == NULL) break;
            item = end + 1;
        }
    }
    free(cookie);
    return found;
}

static bool webui__authenticated(bruce_http_server_request_t *request) {
    char token[BRUCE_CONFIG_WEB_UI_SESSION_TOKEN_LEN + 1u];
    return webui__cookie_token(request, token, sizeof(token)) && config__is_valid_web_ui_session(token);
}

static bool webui__require_auth(bruce_http_server_request_t *request, bruce_result_t *response_result) {
    if (webui__authenticated(request)) return true;
    *response_result = webui__reply_text(request, 401, "Unauthorized");
    return false;
}

static int webui__hex(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool webui__decode(const char *source, size_t length, char *output, size_t capacity) {
    size_t written = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)source[i];
        if (value == '%') {
            if (i + 2u >= length) return false;
            int high = webui__hex(source[i + 1u]);
            int low = webui__hex(source[i + 2u]);
            if (high < 0 || low < 0) return false;
            value = (unsigned char)((high << 4) | low);
            i += 2u;
        } else if (value == '+') {
            value = ' ';
        }
        if (value == 0 || (value < 0x20u && value != '\t' && value != '\n' && value != '\r') ||
            value == 0x7fu || written + 1u >= capacity)
            return false;
        output[written++] = (char)value;
    }
    if (capacity == 0) return false;
    output[written] = '\0';
    return true;
}

static bool webui__form_value(const char *form, const char *key, char *output, size_t capacity) {
    size_t key_len = strlen(key);
    const char *item = form;
    while (*item != '\0') {
        const char *end = strchr(item, '&');
        if (end == NULL) end = item + strlen(item);
        const char *equals = memchr(item, '=', (size_t)(end - item));
        if (equals != NULL && (size_t)(equals - item) == key_len && memcmp(item, key, key_len) == 0)
            return webui__decode(equals + 1, (size_t)(end - equals - 1), output, capacity);
        if (*end == '\0') break;
        item = end + 1;
    }
    return false;
}

static char *webui__read_query(bruce_http_server_request_t *request) {
    size_t length = http_server_request__query_length(request);
    if (length > WEBUI_QUERY_MAX) return NULL;
    char *query = malloc(length + 1u);
    if (query == NULL) return NULL;
    if (http_server_request__get_query(request, query, length + 1u) != BRUCE_OK) {
        free(query);
        return NULL;
    }
    return query;
}

static char *webui__read_body(bruce_http_server_request_t *request, size_t maximum) {
    size_t length = http_server_request__content_length(request);
    if (length > maximum) return NULL;
    char *body = malloc(length + 1u);
    if (body == NULL) return NULL;
    size_t received = 0;
    while (received < length) {
        int count = http_server_request__recv(request, body + received, length - received);
        if (count <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)count;
    }
    if (memchr(body, '\0', length) != NULL) {
        free(body);
        return NULL;
    }
    body[length] = '\0';
    return body;
}

static bool
webui__storage_path(const char *fs, const char *folder, const char *name, char *output, size_t capacity) {
    const char *prefix;
    if (strcmp(fs, "LittleFS") == 0) prefix = "";
    else if (strcmp(fs, "SD") == 0) prefix = "/sdcard";
    else return false;

    char combined[BRUCE_STORAGE_PATH_MAX];
    if (folder == NULL || *folder == '\0') folder = "/";
    int length;
    if (name == NULL || *name == '\0') length = snprintf(combined, sizeof(combined), "%s", folder);
    else
        length = snprintf(
            combined, sizeof(combined), "%s%s%s", folder, folder[strlen(folder) - 1u] == '/' ? "" : "/", name
        );
    if (length < 0 || (size_t)length >= sizeof(combined) || combined[0] != '/') return false;

    size_t written = strlen(prefix);
    if (written + 1u >= capacity) return false;
    memcpy(output, prefix, written);
    output[written++] = '/';
    output[written] = '\0';

    const char *component = combined + 1;
    while (*component != '\0') {
        const char *slash = strchr(component, '/');
        size_t component_len = slash != NULL ? (size_t)(slash - component) : strlen(component);
        bool has_control = false;
        for (size_t i = 0; i < component_len; i++) {
            unsigned char value = (unsigned char)component[i];
            if (value < 0x20u || value == 0x7fu) has_control = true;
        }
        if (component_len == 0 || has_control || (component_len == 1 && component[0] == '.') ||
            (component_len == 2 && component[0] == '.' && component[1] == '.') ||
            memchr(component, '\\', component_len) != NULL)
            return false;
        if (written > 1u && output[written - 1u] != '/') {
            if (written + 1u >= capacity) return false;
            output[written++] = '/';
        }
        if (written + component_len >= capacity) return false;
        memcpy(output + written, component, component_len);
        written += component_len;
        output[written] = '\0';
        if (slash == NULL) break;
        component = slash + 1;
    }
    if (strcmp(prefix, "/sdcard") == 0 && written == 8u) output[7] = '\0';
    return true;
}

static void webui__human_size(size_t bytes, char *output, size_t capacity) {
    if (bytes < 1024u) snprintf(output, capacity, "%u B", (unsigned)bytes);
    else if (bytes < 1024u * 1024u) snprintf(output, capacity, "%.1f kB", (double)bytes / 1024.0);
    else if (bytes < 1024u * 1024u * 1024u)
        snprintf(output, capacity, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else snprintf(output, capacity, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

static bruce_result_t webui__root(bruce_http_server_request_t *request, void *context) {
    (void)context;
    return webui__serve_asset(request, webui__authenticated(request) ? &s_index_asset : &s_login_asset);
}

static bruce_result_t webui__asset(bruce_http_server_request_t *request, void *context) {
    return webui__serve_asset(request, context);
}

static bruce_result_t webui__theme(bruce_http_server_request_t *request, void *context) {
    (void)context;
    uint16_t colors[] = {config__get_pri_color(), config__get_sec_color(), config__get_bg_color()};
    char css[96];
    char *cursor = css;
    size_t left = sizeof(css);
    int count = snprintf(cursor, left, ":root{");
    if (count < 0 || (size_t)count >= left) return webui__reply_text(request, 500, "Theme failed");
    cursor += count;
    left -= (size_t)count;
    const char *names[] = {"--color", "--sec-color", "--background"};
    for (size_t i = 0; i < 3u; i++) {
        uint16_t color = colors[i];
        unsigned r = ((color >> 11) & 0x1fu) * 255u / 31u;
        unsigned g = ((color >> 5) & 0x3fu) * 255u / 63u;
        unsigned b = (color & 0x1fu) * 255u / 31u;
        count = snprintf(cursor, left, "%s:#%02X%02X%02X;", names[i], r, g, b);
        if (count < 0 || (size_t)count >= left) return webui__reply_text(request, 500, "Theme failed");
        cursor += count;
        left -= (size_t)count;
    }
    if (left < 2u) return webui__reply_text(request, 500, "Theme failed");
    *cursor++ = '}';
    *cursor = '\0';
    return webui__reply(request, 200, "text/css; charset=utf-8", css, (size_t)(cursor - css));
}

static bruce_result_t webui__redirect(bruce_http_server_request_t *request, const char *location) {
    bruce_result_t result = http_server_request__set_status(request, 302);
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Location", location);
    if (result == BRUCE_OK) result = http_server_request__send(request, NULL, 0);
    return result;
}

static bruce_result_t webui__login(bruce_http_server_request_t *request, void *context) {
    (void)context;
    char *body = webui__read_body(request, 1024u);
    if (body == NULL) return webui__reply_text(request, 413, "Login form too large");
    char username[65];
    char password[65];
    bool valid = webui__form_value(body, "username", username, sizeof(username)) &&
                 webui__form_value(body, "password", password, sizeof(password));
    free(body);
    const char *expected_user = config__get_web_ui_user();
    const char *expected_password = config__get_web_ui_password();
    valid = valid && expected_user != NULL && expected_password != NULL &&
            strcmp(username, expected_user) == 0 && strcmp(password, expected_password) == 0;
    if (!valid) return webui__redirect(request, "/?failed");

    char token[BRUCE_CONFIG_WEB_UI_SESSION_TOKEN_LEN + 1u];
    bruce_result_t result = config__create_web_ui_session(token, sizeof(token));
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    char cookie[128];
    int length =
        snprintf(cookie, sizeof(cookie), "BRUCESESSION=%s; Path=/; HttpOnly; SameSite=Strict", token);
    if (length < 0 || (size_t)length >= sizeof(cookie))
        return webui__reply_text(request, 500, "Login failed");
    result = http_server_request__set_status(request, 302);
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Location", "/");
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Set-Cookie", cookie);
    if (result == BRUCE_OK) result = http_server_request__send(request, NULL, 0);
    return result;
}

static bruce_result_t webui__logout(bruce_http_server_request_t *request, void *context) {
    (void)context;
    char token[BRUCE_CONFIG_WEB_UI_SESSION_TOKEN_LEN + 1u];
    if (webui__cookie_token(request, token, sizeof(token))) (void)config__remove_web_ui_session(token);
    bruce_result_t result = http_server_request__set_status(request, 302);
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Location", "/?loggedout");
    if (result == BRUCE_OK)
        result = http_server_request__set_header(
            request, "Set-Cookie", "BRUCESESSION=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"
        );
    if (result == BRUCE_OK) result = http_server_request__send(request, NULL, 0);
    return result;
}

static bruce_result_t webui__system_info(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    size_t fs_total = 0, fs_used = 0, sd_total = 0, sd_used = 0;
    bruce_result_t fs_result = storage__get_usage("/", &fs_total, &fs_used);
    bruce_result_t sd_result = storage__get_usage("/sdcard", &sd_total, &sd_used);
    if (fs_result != BRUCE_OK) return webui__reply_error(request, fs_result);
    if (sd_result != BRUCE_OK) sd_total = sd_used = 0;
    char fs_total_text[24], fs_used_text[24], fs_free_text[24];
    char sd_total_text[24], sd_used_text[24], sd_free_text[24];
    webui__human_size(fs_total, fs_total_text, sizeof(fs_total_text));
    webui__human_size(fs_used, fs_used_text, sizeof(fs_used_text));
    webui__human_size(fs_total - fs_used, fs_free_text, sizeof(fs_free_text));
    webui__human_size(sd_total, sd_total_text, sizeof(sd_total_text));
    webui__human_size(sd_used, sd_used_text, sizeof(sd_used_text));
    webui__human_size(sd_total - sd_used, sd_free_text, sizeof(sd_free_text));
    char json[384];
    int length = snprintf(
        json,
        sizeof(json),
        "{\"BRUCE_VERSION\":\"BruceIDF\",\"SD\":{\"free\":\"%s\",\"used\":\"%s\",\"total\":\"%s\"},"
        "\"LittleFS\":{\"free\":\"%s\",\"used\":\"%s\",\"total\":\"%s\"}}",
        sd_free_text,
        sd_used_text,
        sd_total_text,
        fs_free_text,
        fs_used_text,
        fs_total_text
    );
    if (length < 0 || (size_t)length >= sizeof(json)) return webui__reply_text(request, 500, "JSON failed");
    return webui__reply(request, 200, "application/json", json, (size_t)length);
}

static bruce_result_t webui__list_files(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *query = webui__read_query(request);
    if (query == NULL) return webui__reply_text(request, 400, "Invalid query");
    char fs[16], folder[BRUCE_STORAGE_PATH_MAX], path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(query, "fs", fs, sizeof(fs)) &&
                 webui__form_value(query, "folder", folder, sizeof(folder)) &&
                 webui__storage_path(fs, folder, NULL, path, sizeof(path));
    free(query);
    if (!valid) return webui__reply_text(request, 400, "Invalid path");

    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    if (count > SIZE_MAX / sizeof(bruce_storage_entry_t))
        return webui__reply_text(request, 503, "Directory too large");
    size_t capacity = count;
    bruce_storage_entry_t *entries = count > 0 ? malloc(count * sizeof(*entries)) : NULL;
    if (count > 0 && entries == NULL) return webui__reply_text(request, 503, "Out of memory");
    result = storage__list(path, entries, count, &count);
    if (result != BRUCE_OK || count > capacity) {
        free(entries);
        return webui__reply_error(request, result != BRUCE_OK ? result : BRUCE_ERR_BUSY);
    }
    result = http_server_request__set_type(request, "text/plain; charset=utf-8");
    if (result == BRUCE_OK) {
        char parent[BRUCE_STORAGE_PATH_MAX + 8u];
        int length = snprintf(parent, sizeof(parent), "pa:%s:0\n", folder);
        if (length > 0 && (size_t)length < sizeof(parent))
            result = http_server_request__send_chunk(request, parent, (size_t)length);
        else result = BRUCE_ERR_INTERNAL;
    }
    for (size_t i = 0; result == BRUCE_OK && i < count; i++) {
        char size[24];
        webui__human_size(entries[i].size, size, sizeof(size));
        char line[BRUCE_STORAGE_NAME_MAX + 32u];
        int length = snprintf(
            line,
            sizeof(line),
            "%s:%s:%s\n",
            entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY ? "Fo" : "Fi",
            entries[i].name,
            entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY ? "0" : size
        );
        if (length < 0 || (size_t)length >= sizeof(line)) result = BRUCE_ERR_INTERNAL;
        else result = http_server_request__send_chunk(request, line, (size_t)length);
    }
    free(entries);
    if (result == BRUCE_OK) result = http_server_request__finalize(request);
    return result;
}

static bruce_result_t webui__stream_file(
    bruce_http_server_request_t *request, const char *path, const char *type, bool attachment
) {
    bruce_file_id_t file;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    if (attachment) {
        const char *base = strrchr(path, '/');
        base = base != NULL ? base + 1 : path;
        char disposition[BRUCE_STORAGE_NAME_MAX + 32u];
        int length = snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", base);
        if (length < 0 || (size_t)length >= sizeof(disposition)) result = BRUCE_ERR_INVALID_PATH;
        else result = http_server_request__set_header(request, "Content-Disposition", disposition);
    }
    if (result == BRUCE_OK) result = http_server_request__set_type(request, type);
    uint8_t *buffer = result == BRUCE_OK ? malloc(WEBUI_IO_CHUNK) : NULL;
    if (result == BRUCE_OK && buffer == NULL) result = BRUCE_ERR_NO_MEMORY;
    bool response_started = false;
    while (result == BRUCE_OK) {
        size_t count = 0;
        result = storage__read(file, buffer, WEBUI_IO_CHUNK, &count);
        if (result != BRUCE_OK || count == 0) break;
        result = http_server_request__send_chunk(request, buffer, count);
        if (result == BRUCE_OK) response_started = true;
    }
    free(buffer);
    bruce_result_t close_result = storage__close(file);
    if (result == BRUCE_OK && close_result != BRUCE_OK) result = close_result;
    if (result == BRUCE_OK) {
        result = response_started ? http_server_request__finalize(request)
                                  : http_server_request__send(request, NULL, 0);
    }
    if (result != BRUCE_OK && !response_started) return webui__reply_error(request, result);
    return result;
}

static bruce_result_t webui__remove_tree(const char *path, unsigned depth) {
    if (depth > WEBUI_DELETE_DEPTH_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result == BRUCE_ERR_INVALID_ARGUMENT || result == BRUCE_ERR_IO) return storage__remove(path);
    if (result != BRUCE_OK) return result;
    if (count > WEBUI_DELETE_ENTRIES_MAX) return BRUCE_ERR_RESOURCE_LIMIT;
    bruce_storage_entry_t *entries = count > 0 ? malloc(count * sizeof(*entries)) : NULL;
    if (count > 0 && entries == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t capacity = count;
    result = storage__list(path, entries, capacity, &count);
    if (result == BRUCE_OK && count > capacity) result = BRUCE_ERR_BUSY;
    for (size_t i = 0; result == BRUCE_OK && i < count; i++) {
        char child[BRUCE_STORAGE_PATH_MAX];
        int length = snprintf(child, sizeof(child), "%s/%s", path, entries[i].name);
        if (length < 0 || (size_t)length >= sizeof(child)) result = BRUCE_ERR_INVALID_PATH;
        else if (entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY)
            result = webui__remove_tree(child, depth + 1u);
        else result = storage__remove(child);
    }
    free(entries);
    return result == BRUCE_OK ? storage__remove(path) : result;
}

static const char *webui__image_type(const char *path) {
    const char *extension = strrchr(path, '.');
    if (extension == NULL) return "application/octet-stream";
    if (strcmp(extension, ".jpg") == 0 || strcmp(extension, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(extension, ".png") == 0) return "image/png";
    if (strcmp(extension, ".gif") == 0) return "image/gif";
    if (strcmp(extension, ".bmp") == 0) return "image/bmp";
    if (strcmp(extension, ".webp") == 0) return "image/webp";
    return "application/octet-stream";
}

static bruce_result_t webui__file(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *query = webui__read_query(request);
    if (query == NULL) return webui__reply_text(request, 400, "Invalid query");
    char fs[16], name[BRUCE_STORAGE_PATH_MAX], action[16], path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(query, "fs", fs, sizeof(fs)) &&
                 webui__form_value(query, "name", name, sizeof(name)) &&
                 webui__form_value(query, "action", action, sizeof(action)) &&
                 webui__storage_path(fs, name, NULL, path, sizeof(path));
    free(query);
    if (!valid) return webui__reply_text(request, 400, "Invalid path");
    if (strcmp(action, "download") == 0)
        return webui__stream_file(request, path, "application/octet-stream", true);
    if (strcmp(action, "image") == 0)
        return webui__stream_file(request, path, webui__image_type(path), false);
    if (strcmp(action, "edit") == 0)
        return webui__stream_file(request, path, "text/plain; charset=utf-8", false);
    bruce_result_t result;
    if (strcmp(action, "create") == 0) result = storage__mkdir(path);
    else if (strcmp(action, "createfile") == 0) {
        bruce_file_id_t file;
        result = storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        );
        if (result == BRUCE_OK) result = storage__close(file);
    } else if (strcmp(action, "delete") == 0) {
        result = webui__remove_tree(path, 0);
    } else return webui__reply_text(request, 400, "Invalid action");
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    return webui__reply_text(request, 200, "OK");
}

static bruce_result_t webui__rename(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *body = webui__read_body(request, 2048u);
    if (body == NULL) return webui__reply_text(request, 413, "Form too large");
    char fs[16], old_name[BRUCE_STORAGE_PATH_MAX], new_name[BRUCE_STORAGE_NAME_MAX];
    char old_path[BRUCE_STORAGE_PATH_MAX], new_path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(body, "fs", fs, sizeof(fs)) &&
                 webui__form_value(body, "filePath", old_name, sizeof(old_name)) &&
                 webui__form_value(body, "fileName", new_name, sizeof(new_name));
    free(body);
    if (!valid || strchr(new_name, '/') != NULL || strcmp(new_name, ".") == 0 ||
        strcmp(new_name, "..") == 0 || !webui__storage_path(fs, old_name, NULL, old_path, sizeof(old_path)))
        return webui__reply_text(request, 400, "Invalid path");
    char browser_parent[BRUCE_STORAGE_PATH_MAX];
    snprintf(browser_parent, sizeof(browser_parent), "%s", old_name);
    char *slash = strrchr(browser_parent, '/');
    if (slash == NULL) return webui__reply_text(request, 400, "Invalid path");
    if (slash == browser_parent) slash[1] = '\0';
    else *slash = '\0';
    if (!webui__storage_path(fs, browser_parent, new_name, new_path, sizeof(new_path)))
        return webui__reply_text(request, 400, "Invalid path");
    bruce_result_t result = storage__rename(old_path, new_path);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    return webui__reply_text(request, 200, "OK");
}

static bruce_result_t webui__edit(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *body = webui__read_body(request, WEBUI_FORM_MAX * 3u + 1024u);
    if (body == NULL) return webui__reply_text(request, 413, "Editor content exceeds 64 KiB");
    char fs[16], name[BRUCE_STORAGE_PATH_MAX], path[BRUCE_STORAGE_PATH_MAX];
    char *content = malloc(WEBUI_FORM_MAX + 1u);
    bool valid = content != NULL && webui__form_value(body, "fs", fs, sizeof(fs)) &&
                 webui__form_value(body, "name", name, sizeof(name)) &&
                 webui__form_value(body, "content", content, WEBUI_FORM_MAX + 1u) &&
                 webui__storage_path(fs, name, NULL, path, sizeof(path));
    free(body);
    if (!valid) {
        free(content);
        return webui__reply_text(request, 400, "Invalid editor form");
    }
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    size_t length = strlen(content), offset = 0;
    while (result == BRUCE_OK && offset < length) {
        size_t written = 0;
        result = storage__write(file, content + offset, length - offset, &written);
        if (result == BRUCE_OK && written == 0) result = BRUCE_ERR_IO;
        offset += written;
    }
    free(content);
    if (result == BRUCE_OK) result = storage__close(file);
    else if (file != BRUCE_FILE_ID_INVALID) (void)storage__close(file);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    return webui__reply_text(request, 200, "OK");
}

static bruce_result_t webui__mkdir_parents(char *path) {
    for (char *slash = path + 1; *slash != '\0'; slash++) {
        if (*slash != '/') continue;
        *slash = '\0';
        bruce_result_t result = storage__mkdir(path);
        *slash = '/';
        if (result != BRUCE_OK) return result;
    }
    return BRUCE_OK;
}

static bruce_result_t webui__upload(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    size_t content_length = http_server_request__content_length(request);
    if (content_length > WEBUI_UPLOAD_MAX) return webui__reply_text(request, 413, "Upload exceeds 8 MiB");
    char *query = webui__read_query(request);
    if (query == NULL) return webui__reply_text(request, 400, "Invalid query");
    char fs[16], folder[BRUCE_STORAGE_PATH_MAX], name[BRUCE_STORAGE_PATH_MAX], path[BRUCE_STORAGE_PATH_MAX];
    bool valid = webui__form_value(query, "fs", fs, sizeof(fs)) &&
                 webui__form_value(query, "folder", folder, sizeof(folder)) &&
                 webui__form_value(query, "name", name, sizeof(name)) && name[0] != '/' &&
                 webui__storage_path(fs, folder, name, path, sizeof(path));
    free(query);
    if (!valid) return webui__reply_text(request, 400, "Invalid upload path");
    bruce_result_t result = webui__mkdir_parents(path);
    bruce_file_id_t file = 0;
    if (result == BRUCE_OK)
        result = storage__open(
            path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        );
    uint8_t *buffer = result == BRUCE_OK ? malloc(WEBUI_IO_CHUNK) : NULL;
    if (result == BRUCE_OK && buffer == NULL) result = BRUCE_ERR_NO_MEMORY;
    size_t received = 0;
    while (result == BRUCE_OK && received < content_length) {
        size_t wanted = content_length - received;
        if (wanted > WEBUI_IO_CHUNK) wanted = WEBUI_IO_CHUNK;
        int count = http_server_request__recv(request, buffer, wanted);
        if (count <= 0) {
            result = count < 0 ? (bruce_result_t)count : BRUCE_ERR_IO;
            break;
        }
        size_t offset = 0;
        while (result == BRUCE_OK && offset < (size_t)count) {
            size_t written = 0;
            result = storage__write(file, buffer + offset, (size_t)count - offset, &written);
            if (result == BRUCE_OK && written == 0) result = BRUCE_ERR_IO;
            offset += written;
        }
        received += (size_t)count;
    }
    free(buffer);
    if (file != 0) {
        bruce_result_t close_result = storage__close(file);
        if (result == BRUCE_OK) result = close_result;
    }
    if (result != BRUCE_OK) {
        if (file != BRUCE_FILE_ID_INVALID) (void)storage__remove(path);
        return webui__reply_error(request, result);
    }
    return webui__reply_text(request, 200, "Upload complete");
}

static bruce_result_t webui__command(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *body = webui__read_body(request, 2048u);
    if (body == NULL) return webui__reply_text(request, 413, "Command too large");
    char command[1024];
    bool valid = webui__form_value(body, "cmnd", command, sizeof(command));
    free(body);
    if (!valid) return webui__reply_text(request, 400, "Missing command");
    char *cursor = command;
    while (isspace((unsigned char)*cursor)) cursor++;
    if (strncmp(cursor, "nav", 3) == 0 && isspace((unsigned char)cursor[3])) {
        cursor += 3;
        while (isspace((unsigned char)*cursor)) cursor++;
        char key[16];
        size_t key_len = 0;
        while (cursor[key_len] != '\0' && !isspace((unsigned char)cursor[key_len]) &&
               key_len + 1u < sizeof(key)) {
            key[key_len] = (char)tolower((unsigned char)cursor[key_len]);
            key_len++;
        }
        key[key_len] = '\0';
        int32_t code = 0;
        if (strcmp(key, "sel") == 0 || strcmp(key, "select") == 0) code = BRUCE_INPUT_CODE_SELECT;
        else if (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) code = BRUCE_INPUT_CODE_BACK;
        else if (strcmp(key, "up") == 0) code = BRUCE_INPUT_CODE_UP;
        else if (strcmp(key, "down") == 0) code = BRUCE_INPUT_CODE_DOWN;
        else if (strcmp(key, "next") == 0 || strcmp(key, "nextpage") == 0) code = BRUCE_INPUT_CODE_RIGHT;
        else if (strcmp(key, "prev") == 0 || strcmp(key, "prevpage") == 0) code = BRUCE_INPUT_CODE_LEFT;
        else if (strcmp(key, "menu") == 0) code = BRUCE_INPUT_CODE_MENU;
        else return webui__reply_text(request, 400, "Unknown navigation key");
        bruce_input_event_t event = {
            .type = BRUCE_INPUT_KEY, .action = BRUCE_INPUT_PRESS, .code = code, .value = 1
        };
        bruce_result_t result = input__inject(&event);
        event.action = BRUCE_INPUT_RELEASE;
        event.value = 0;
        if (result == BRUCE_OK) result = input__inject(&event);
        if (result != BRUCE_OK) return webui__reply_error(request, result);
        return webui__reply_text(request, 200, "OK");
    }
    char *name = cursor;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor)) cursor++;
    if (cursor == name) return webui__reply_text(request, 400, "Missing command");
    char *argument = cursor;
    if (*cursor != '\0') {
        *cursor++ = '\0';
        while (isspace((unsigned char)*cursor)) cursor++;
        argument = cursor;
    } else argument = NULL;
    int task = app_runner__run(name, argument, true);
    if (task < 0) return webui__reply_error(request, (bruce_result_t)task);
    return webui__reply_text(request, 202, "Command queued");
}

static bruce_result_t webui__wifi(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *form = NULL;
    if (http_server_request__content_length(request) > 0) form = webui__read_body(request, 1024u);
    else form = webui__read_query(request);
    if (form == NULL) return webui__reply_text(request, 400, "Invalid credentials form");
    char user[65], password[65];
    bool valid = webui__form_value(form, "usr", user, sizeof(user)) &&
                 webui__form_value(form, "pwd", password, sizeof(password));
    free(form);
    if (!valid || user[0] == '\0' || password[0] == '\0')
        return webui__reply_text(request, 400, "Username and password required");
    bruce_result_t result = config__set_web_ui_user(user);
    if (result == BRUCE_OK) result = config__set_web_ui_password(password);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    return webui__reply_text(request, 200, "Credentials updated");
}

static void webui__le16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void webui__le32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static bruce_result_t webui__screen(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    uint16_t width = 0, height = 0;
    size_t pixel_count = 0;
    bruce_result_t result = display__snapshot(NULL, 0, &width, &height, &pixel_count);
    if (result != BRUCE_OK || width == 0 || height == 0 || pixel_count != (size_t)width * height)
        return webui__reply_error(request, result != BRUCE_OK ? result : BRUCE_ERR_INVALID_STATE);
    if (pixel_count > SIZE_MAX / sizeof(uint16_t)) return webui__reply_text(request, 503, "Screen too large");
    uint16_t *pixels = malloc(pixel_count * sizeof(*pixels));
    if (pixels == NULL) return webui__reply_text(request, 503, "Screen memory unavailable");
    result = display__snapshot(pixels, pixel_count, &width, &height, &pixel_count);
    size_t row_size = ((size_t)width * 3u + 3u) & ~(size_t)3u;
    if (result != BRUCE_OK || row_size > UINT32_MAX || (size_t)height > (UINT32_MAX - 54u) / row_size) {
        free(pixels);
        return webui__reply_error(request, result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT);
    }
    uint8_t *row = calloc(1, row_size);
    if (row == NULL) {
        free(pixels);
        return webui__reply_text(request, 503, "Screen memory unavailable");
    }
    uint32_t image_size = (uint32_t)(row_size * height);
    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    webui__le32(header + 2, 54u + image_size);
    webui__le32(header + 10, 54u);
    webui__le32(header + 14, 40u);
    webui__le32(header + 18, width);
    webui__le32(header + 22, height);
    webui__le16(header + 26, 1u);
    webui__le16(header + 28, 24u);
    webui__le32(header + 34, image_size);
    result = http_server_request__set_type(request, "image/bmp");
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Cache-Control", "no-store");
    if (result == BRUCE_OK) result = http_server_request__send_chunk(request, header, sizeof(header));
    for (size_t y = height; result == BRUCE_OK && y > 0; y--) {
        memset(row, 0, row_size);
        const uint16_t *source = pixels + (y - 1u) * width;
        for (size_t x = 0; x < width; x++) {
            uint16_t color = source[x];
            row[x * 3u] = (uint8_t)(((color & 0x1fu) * 255u) / 31u);
            row[x * 3u + 1u] = (uint8_t)((((color >> 5) & 0x3fu) * 255u) / 63u);
            row[x * 3u + 2u] = (uint8_t)((((color >> 11) & 0x1fu) * 255u) / 31u);
        }
        result = http_server_request__send_chunk(request, row, row_size);
    }
    free(row);
    free(pixels);
    if (result == BRUCE_OK) result = http_server_request__finalize(request);
    return result;
}

static bruce_result_t webui__reboot(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    bruce_result_t result = webui__reply_text(request, 200, "Restarting");
    if (result == BRUCE_OK) (void)device__restart(250);
    return result;
}

static bruce_result_t webui__api_status(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    bruce_http_server_status_t status;
    bruce_result_t result = http_server__get_status(&status);
    if (result != BRUCE_OK) return webui__reply_error(request, result);
    const char *ip = wifi__get_ip();
    char json[192];
    int length = snprintf(
        json,
        sizeof(json),
        "{\"running\":%s,\"mode\":\"%s\",\"ip\":\"%s\",\"port\":%u}",
        status.running ? "true" : "false",
        s_network_mode == WEBUI_APP_NETWORK_AP ? "ap" : "existing",
        ip != NULL ? ip : "",
        status.port
    );
    if (length < 0 || (size_t)length >= sizeof(json)) return webui__reply_text(request, 500, "JSON failed");
    return webui__reply(request, 200, "application/json", json, (size_t)length);
}

static const bruce_http_server_route_t s_routes[] = {
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/", .callback = webui__root},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/login", .callback = webui__login},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/logout", .callback = webui__logout},
    {.method = BRUCE_HTTP_SERVER_GET,
     .uri = "/index.css",
     .callback = webui__asset,
     .context = (void *)&s_css_asset},
    {.method = BRUCE_HTTP_SERVER_GET,
     .uri = "/index.js",
     .callback = webui__asset,
     .context = (void *)&s_js_asset},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/theme.css", .callback = webui__theme},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/systeminfo", .callback = webui__system_info},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/listfiles", .callback = webui__list_files},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/file", .callback = webui__file},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/rename", .callback = webui__rename},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/edit", .callback = webui__edit},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/upload", .callback = webui__upload},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/cm", .callback = webui__command},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/wifi", .callback = webui__wifi},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/wifi", .callback = webui__wifi},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/getscreen", .callback = webui__screen},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/reboot", .callback = webui__reboot},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/api/status", .callback = webui__api_status},
};

static int webui_app__status(bool gui) {
    bruce_http_server_status_t status;
    bruce_result_t result = http_server__get_status(&status);
    if (result != BRUCE_OK) return result;
    char message[160];
    if (status.running) {
        const char *ip = wifi__get_ip();
        snprintf(
            message,
            sizeof(message),
            "Running on http://%s:%u\nNetwork: %s",
            ip != NULL ? ip : "unknown",
            status.port,
            s_network_mode == WEBUI_APP_NETWORK_AP ? "access point" : "existing Wi-Fi"
        );
    } else snprintf(message, sizeof(message), "WebUI is stopped");
    if (gui) (void)dialog__message(BRUCE_DIALOG_INFO, "WebUI", message);
    else stdio__printf("%s\n", message);
    return BRUCE_OK;
}

static int webui_app__start(webui_app_network_mode_t mode, bool gui) {
    bruce_result_t result;
    if (mode == WEBUI_APP_NETWORK_AP) result = wifi__is_ap_running() ? BRUCE_OK : wifi__setup_ap();
    else result = wifi__is_connected() ? BRUCE_OK : wifi__connect_known();
    if (result != BRUCE_OK) {
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not start Wi-Fi");
        else stdio__printf("Wi-Fi start failed: %d\n", result);
        return result;
    }
    const bruce_http_server_options_t options = {
        .port = 80, .routes = s_routes, .route_count = sizeof(s_routes) / sizeof(s_routes[0])
    };
    s_network_mode = mode;
    result = http_server__start(&options);
    if (result != BRUCE_OK) {
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not start WebUI");
        else stdio__printf("WebUI start failed: %d\n", result);
        return result;
    }
    return webui_app__status(gui);
}

static int webui_app__gui(void) {
    for (;;) {
        bool running = http_server__is_running();
        const bruce_dialog_choice_t choices[] = {
            {.label = running ? "Stop server" : "Start server", .value = "toggle"   },
            {.label = "Exit",                                   .value = "exit"     },
            {.label = "Stop and exit",                          .value = "stop-exit"},
        };
        size_t selected = 0;
        bruce_result_t result = dialog__choice("WebUI", "Browser access", choices, 3, &selected, NULL);
        if (result == BRUCE_ERR_CANCELLED || selected == 1u) return BRUCE_OK;
        if (result != BRUCE_OK) return result;
        if (selected == 2u) {
            if (http_server__is_running()) return http_server__stop();
            return BRUCE_OK;
        }
        if (running) {
            result = http_server__stop();
            if (result != BRUCE_OK)
                (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not stop server");
            continue;
        }
        if (wifi__is_connected()) {
            (void)webui_app__start(WEBUI_APP_NETWORK_EXISTING, true);
            continue;
        }
        if (wifi__is_ap_running()) {
            (void)webui_app__start(WEBUI_APP_NETWORK_AP, true);
            continue;
        }
        if (wifi__connect_known() == BRUCE_OK) {
            (void)webui_app__start(WEBUI_APP_NETWORK_EXISTING, true);
            continue;
        }
        const bruce_dialog_choice_t network_choices[] = {
            {.label = "Start access point", .value = "ap"    },
            {.label = "Cancel",             .value = "cancel"},
        };
        size_t network = 0;
        result = dialog__choice("Start WebUI", "Existing Wi-Fi unavailable", network_choices, 2, &network, NULL);
        if (result == BRUCE_ERR_CANCELLED || (result == BRUCE_OK && network == 1u)) continue;
        if (result != BRUCE_OK) return result;
        (void)webui_app__start(WEBUI_APP_NETWORK_AP, true);
    }
}

static void webui_app__print_help(void) { stdio__printf("Usage: webui [status|stop|start ap|start sta]\n"); }

int webui_app_main(int argc, char **argv) {
    if (app_runner__args_have_gui(argc, argv)) {
        if (!app_runner__args_have_background(argc, argv)) {
            bruce_result_t foreground = task__to_foreground();
            if (foreground != BRUCE_OK) return foreground;
        }
        return webui_app__gui();
    }
    if (argc <= 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) return webui_app__status(false);
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        bruce_result_t result = http_server__stop();
        if (result == BRUCE_OK) stdio__printf("WebUI stopped\n");
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "start") == 0) {
        if (strcmp(argv[2], "ap") == 0) return webui_app__start(WEBUI_APP_NETWORK_AP, false);
        if (strcmp(argv[2], "sta") == 0) return webui_app__start(WEBUI_APP_NETWORK_EXISTING, false);
    }
    webui_app__print_help();
    return BRUCE_ERR_INVALID_ARGUMENT;
}
