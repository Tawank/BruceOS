#include "webui_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"

#include "core_sdk/app_config.h"
#include "core_sdk/memory.h"

#define WEBUI_CREDENTIAL_MAX_LEN 64u
#define WEBUI_SESSION_TOKEN_LEN 48u
#define WEBUI_MAX_SESSIONS 5u

static bool webui__cookie_token(bruce_http_server_request_t *request, char *token, size_t capacity) {
    size_t length = http_server_request__header_length(request, "Cookie");
    if (length == 0 || length > 1024u) return false;
    char *cookie = memory__malloc(length + 1u);
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
    memory__free(cookie);
    return found;
}

/* Reads the "sessions" string array from /config/webui.conf into a fixed
 * WEBUI_MAX_SESSIONS x (WEBUI_SESSION_TOKEN_LEN + 1) buffer. */
static size_t webui__load_sessions(char sessions[][WEBUI_SESSION_TOKEN_LEN + 1u]) {
    char *slots[WEBUI_MAX_SESSIONS];
    for (size_t i = 0; i < WEBUI_MAX_SESSIONS; ++i) slots[i] = sessions[i];
    return app_config__get_string_array(
        WEBUI_CONFIG_APP_NAME, "sessions", slots, WEBUI_SESSION_TOKEN_LEN + 1u, WEBUI_MAX_SESSIONS
    );
}

static bool webui__is_valid_session(const char *token) {
    char sessions[WEBUI_MAX_SESSIONS][WEBUI_SESSION_TOKEN_LEN + 1u];
    size_t count = webui__load_sessions(sessions);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(sessions[i], token) == 0) return true;
    }
    return false;
}

/* Generates a random session token, appends it (FIFO-evicting the oldest
 * once WEBUI_MAX_SESSIONS is reached), and persists the session list. */
static bruce_result_t webui__create_session(char *out_token, size_t capacity) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (out_token == NULL || capacity < WEBUI_SESSION_TOKEN_LEN + 1u) return BRUCE_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < WEBUI_SESSION_TOKEN_LEN; ++i) {
        out_token[i] = alphabet[esp_random() % (sizeof(alphabet) - 1u)];
    }
    out_token[WEBUI_SESSION_TOKEN_LEN] = '\0';

    char sessions[WEBUI_MAX_SESSIONS][WEBUI_SESSION_TOKEN_LEN + 1u];
    size_t count = webui__load_sessions(sessions);
    const char *values[WEBUI_MAX_SESSIONS];
    size_t start = count == WEBUI_MAX_SESSIONS ? 1u : 0u;
    size_t new_count = 0;
    for (size_t i = start; i < count; ++i) values[new_count++] = sessions[i];
    values[new_count++] = out_token;
    return app_config__set_string_array(WEBUI_CONFIG_APP_NAME, "sessions", values, new_count);
}

static void webui__remove_session(const char *token) {
    char sessions[WEBUI_MAX_SESSIONS][WEBUI_SESSION_TOKEN_LEN + 1u];
    size_t count = webui__load_sessions(sessions);
    const char *values[WEBUI_MAX_SESSIONS];
    size_t new_count = 0;
    bool removed = false;
    for (size_t i = 0; i < count; ++i) {
        if (!removed && strcmp(sessions[i], token) == 0) {
            removed = true;
            continue;
        }
        values[new_count++] = sessions[i];
    }
    if (removed) (void)app_config__set_string_array(WEBUI_CONFIG_APP_NAME, "sessions", values, new_count);
}

bool webui__authenticated(bruce_http_server_request_t *request) {
    char token[WEBUI_SESSION_TOKEN_LEN + 1u];
    return webui__cookie_token(request, token, sizeof(token)) && webui__is_valid_session(token);
}

bool webui__require_auth(bruce_http_server_request_t *request, bruce_result_t *response_result) {
    if (webui__authenticated(request)) return true;
    *response_result = webui__reply_text(request, 401, "Unauthorized");
    return false;
}

bruce_result_t webui__login(bruce_http_server_request_t *request, void *context) {
    (void)context;
    char *body = webui__read_body(request, 1024u);
    if (body == NULL) return webui__reply_text(request, 413, "Login form too large");
    char username[65];
    char password[65];
    bool valid = webui__form_value(body, "username", username, sizeof(username)) &&
                 webui__form_value(body, "password", password, sizeof(password));
    memory__free(body);
    char expected_user[WEBUI_CREDENTIAL_MAX_LEN + 1u];
    char expected_password[WEBUI_CREDENTIAL_MAX_LEN + 1u];
    app_config__get_string(WEBUI_CONFIG_APP_NAME, "user", "admin", expected_user, sizeof(expected_user));
    app_config__get_string(
        WEBUI_CONFIG_APP_NAME, "password", "bruce", expected_password, sizeof(expected_password)
    );
    valid = valid && strcmp(username, expected_user) == 0 && strcmp(password, expected_password) == 0;
    if (!valid) return webui__redirect(request, "/?failed");

    char token[WEBUI_SESSION_TOKEN_LEN + 1u];
    bruce_result_t result = webui__create_session(token, sizeof(token));
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

bruce_result_t webui__logout(bruce_http_server_request_t *request, void *context) {
    (void)context;
    char token[WEBUI_SESSION_TOKEN_LEN + 1u];
    if (webui__cookie_token(request, token, sizeof(token))) webui__remove_session(token);
    bruce_result_t result = http_server_request__set_status(request, 302);
    if (result == BRUCE_OK) result = http_server_request__set_header(request, "Location", "/?loggedout");
    if (result == BRUCE_OK)
        result = http_server_request__set_header(
            request, "Set-Cookie", "BRUCESESSION=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"
        );
    if (result == BRUCE_OK) result = http_server_request__send(request, NULL, 0);
    return result;
}
