#include "webui_internal.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

bruce_result_t webui__reply(
    bruce_http_server_request_t *request, int status, const char *type, const void *body, size_t body_len
) {
    bruce_result_t result = http_server_request__set_status(request, status);
    if (result == BRUCE_OK) result = http_server_request__set_type(request, type);
    if (result == BRUCE_OK) result = http_server_request__send(request, body, body_len);
    return result;
}

bruce_result_t webui__reply_text(bruce_http_server_request_t *request, int status, const char *body) {
    return webui__reply(request, status, "text/plain; charset=utf-8", body, strlen(body));
}

bruce_result_t webui__reply_error(bruce_http_server_request_t *request, bruce_result_t error) {
    if (error == BRUCE_ERR_PERMISSION) return webui__reply_text(request, 403, "Forbidden");
    if (error == BRUCE_ERR_NOT_FOUND) return webui__reply_text(request, 404, "Not found");
    if (error == BRUCE_ERR_INVALID_ARGUMENT || error == BRUCE_ERR_INVALID_PATH)
        return webui__reply_text(request, 400, "Invalid request");
    if (error == BRUCE_ERR_NO_MEMORY || error == BRUCE_ERR_RESOURCE_LIMIT)
        return webui__reply_text(request, 503, "Resource unavailable");
    if (error == BRUCE_ERR_ALREADY_EXISTS) return webui__reply_text(request, 409, "Already exists");
    return webui__reply_text(request, 500, "Operation failed");
}

bruce_result_t webui__serve_asset(bruce_http_server_request_t *request, const webui_asset_t *asset) {
    bruce_result_t result = http_server_request__set_type(request, asset->type);
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Content-Encoding", "gzip");
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Cache-Control", "no-cache");
    if (result == BRUCE_OK) result = http_server_request__send(request, asset->data, asset->size);
    return result;
}

bruce_result_t webui__redirect(bruce_http_server_request_t *request, const char *location) {
    bruce_result_t result = http_server_request__set_status(request, 302);
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Location", location);
    if (result == BRUCE_OK) result = http_server_request__send(request, NULL, 0);
    return result;
}

int webui__hex(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool webui__decode(const char *source, size_t length, char *output, size_t capacity) {
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

bool webui__form_span(const char *form, const char *key, const char **out_start, size_t *out_length) {
    size_t key_len = strlen(key);
    const char *item = form;
    while (*item != '\0') {
        const char *end = strchr(item, '&');
        if (end == NULL) end = item + strlen(item);
        const char *equals = memchr(item, '=', (size_t)(end - item));
        if (equals != NULL && (size_t)(equals - item) == key_len && memcmp(item, key, key_len) == 0) {
            *out_start = equals + 1;
            *out_length = (size_t)(end - equals - 1);
            return true;
        }
        if (*end == '\0') break;
        item = end + 1;
    }
    return false;
}

bool webui__form_value(const char *form, const char *key, char *output, size_t capacity) {
    const char *start = NULL;
    size_t length = 0;
    if (!webui__form_span(form, key, &start, &length)) return false;
    return webui__decode(start, length, output, capacity);
}

char *webui__read_query(bruce_http_server_request_t *request) {
    size_t length = http_server_request__query_length(request);
    if (length > 1024u) return NULL;
    char *query = memory__malloc(length + 1u);
    if (query == NULL) return NULL;
    if (http_server_request__get_query(request, query, length + 1u) != BRUCE_OK) {
        memory__free(query);
        return NULL;
    }
    return query;
}

char *webui__read_body(bruce_http_server_request_t *request, size_t maximum) {
    size_t length = http_server_request__content_length(request);
    if (length > maximum) return NULL;
    char *body = memory__malloc(length + 1u);
    if (body == NULL) return NULL;
    size_t received = 0;
    while (received < length) {
        int count = http_server_request__recv(request, body + received, length - received);
        if (count <= 0) {
            memory__free(body);
            return NULL;
        }
        received += (size_t)count;
    }
    if (memchr(body, '\0', length) != NULL) {
        memory__free(body);
        return NULL;
    }
    body[length] = '\0';
    return body;
}

bool webui__storage_path(
    const char *fs, const char *folder, const char *name, char *output, size_t capacity
) {
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

void webui__human_size(size_t bytes, char *output, size_t capacity) {
    if (bytes < 1024u) snprintf(output, capacity, "%u B", (unsigned)bytes);
    else if (bytes < 1024u * 1024u) snprintf(output, capacity, "%.1f kB", (double)bytes / 1024.0);
    else if (bytes < 1024u * 1024u * 1024u)
        snprintf(output, capacity, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else snprintf(output, capacity, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}
