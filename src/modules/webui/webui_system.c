#include "webui_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_config.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/config.h"
#include "core_sdk/device.h"
#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/storage.h"
#include "core_sdk/wifi.h"

bruce_result_t webui__theme(bruce_http_server_request_t *request, void *context) {
    (void)context;
    uint16_t colors[] = {
        config__get_theme_primary(), config__get_theme_secondary(), config__get_theme_background()
    };
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

bruce_result_t webui__system_info(bruce_http_server_request_t *request, void *context) {
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

bruce_result_t webui__command(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    char *body = webui__read_body(request, 2048u);
    if (body == NULL) return webui__reply_text(request, 413, "Command too large");
    char command[1024];
    bool valid = webui__form_value(body, "cmnd", command, sizeof(command));
    memory__free(body);
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
    if (*cursor == '\0') return webui__reply_text(request, 400, "Missing command");
    int process = app_runner__run_command(cursor, BRUCE_LAUNCH_BACKGROUND);
    if (process < 0) return webui__reply_error(request, (bruce_result_t)process);
    return webui__reply_text(request, 202, "Command queued");
}

bruce_result_t webui__wifi(bruce_http_server_request_t *request, void *context) {
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
    memory__free(form);
    if (!valid || user[0] == '\0' || password[0] == '\0')
        return webui__reply_text(request, 400, "Username and password required");
    bruce_result_t result = app_config__set_string(WEBUI_CONFIG_APP_NAME, "user", user);
    if (result == BRUCE_OK) result = app_config__set_string(WEBUI_CONFIG_APP_NAME, "password", password);
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

bruce_result_t webui__screen(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    uint16_t width = 0, height = 0;
    size_t pixel_count = 0;
    bruce_result_t result = display__snapshot(NULL, 0, &width, &height, &pixel_count);
    if (result != BRUCE_OK || width == 0 || height == 0 || pixel_count != (size_t)width * height)
        return webui__reply_error(request, result != BRUCE_OK ? result : BRUCE_ERR_INVALID_STATE);
    if (pixel_count > SIZE_MAX / sizeof(uint16_t)) return webui__reply_text(request, 503, "Screen too large");
    void *pixels_data = NULL;
    bruce_memory_object_t pixels_object = {0};
    bool pixels_external = false;
    result = webui__alloc_direct(&pixels_data, &pixels_object, &pixels_external, pixel_count * sizeof(uint16_t));
    if (result != BRUCE_OK) return webui__reply_text(request, 503, "Screen memory unavailable");
    uint16_t *pixels = pixels_data;
    result = display__snapshot(pixels, pixel_count, &width, &height, &pixel_count);
    size_t row_size = ((size_t)width * 3u + 3u) & ~(size_t)3u;
    if (result != BRUCE_OK || row_size > UINT32_MAX || (size_t)height > (UINT32_MAX - 54u) / row_size) {
        webui__free_direct(pixels_data, &pixels_object, pixels_external);
        return webui__reply_error(request, result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT);
    }
    uint8_t *row = memory__calloc(1, row_size);
    if (row == NULL) {
        webui__free_direct(pixels_data, &pixels_object, pixels_external);
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
    memory__free(row);
    webui__free_direct(pixels_data, &pixels_object, pixels_external);
    if (result == BRUCE_OK) result = http_server_request__finalize(request);
    return result;
}

bruce_result_t webui__reboot(bruce_http_server_request_t *request, void *context) {
    (void)context;
    bruce_result_t auth_response;
    if (!webui__require_auth(request, &auth_response)) return auth_response;
    bruce_result_t result = webui__reply_text(request, 200, "Restarting");
    if (result == BRUCE_OK) (void)device__restart(250);
    return result;
}

bruce_result_t webui__api_status(bruce_http_server_request_t *request, void *context) {
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
        webui__get_network_mode() == WEBUI_APP_NETWORK_AP ? "ap" : "existing",
        ip != NULL ? ip : "",
        status.port
    );
    if (length < 0 || (size_t)length >= sizeof(json)) return webui__reply_text(request, 500, "JSON failed");
    return webui__reply(request, 200, "application/json", json, (size_t)length);
}
