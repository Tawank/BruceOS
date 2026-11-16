#include "fakes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/http.h"
#include "core_sdk/icon.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"
#include "core_sdk/wifi.h"

fakes_t fakes;

void fakes__reset(void) {
    memset(&fakes, 0, sizeof(fakes));
    fakes.width = 240;
    fakes.height = 135;
    fakes.decode_size = 256;
    fakes.http_status = 200;
    fakes.http_body_len = 1000;
    fakes.wifi_connected = true;
    fakes.wifi_connect_ok = true;
    fakes.choice_result = BRUCE_ERR_CANCELLED;
}

/* ---- helpers ------------------------------------------------------------ */

static fake_file_t *fakes__file(const char *path) {
    for (size_t i = 0; i < FAKE_FILES_MAX; ++i) {
        if (fakes.files[i].used && strcmp(fakes.files[i].path, path) == 0) return &fakes.files[i];
    }
    return NULL;
}

void fakes__add_file(const char *path, size_t size) {
    for (size_t i = 0; i < FAKE_FILES_MAX; ++i) {
        if (!fakes.files[i].used) {
            fakes.files[i] = (fake_file_t){.used = true, .size = size};
            snprintf(fakes.files[i].path, sizeof(fakes.files[i].path), "%s", path);
            return;
        }
    }
}

bool fakes__file_exists(const char *path) { return fakes__file(path) != NULL; }

size_t fakes__count(fake_draw_kind_t kind) {
    size_t count = 0;
    for (size_t i = 0; i < fakes.draw_count; ++i) count += fakes.draws[i].kind == kind;
    return count;
}

const fake_draw_t *fakes__find_string(const char *text) {
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        if (fakes.draws[i].kind == FAKE_STRING && strcmp(fakes.draws[i].text, text) == 0) return &fakes.draws[i];
    }
    return NULL;
}

const fake_draw_t *fakes__find_prefix(const char *prefix) {
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        const fake_draw_t *draw = &fakes.draws[i];
        if (draw->kind == FAKE_STRING && strncmp(draw->text, prefix, strlen(prefix)) == 0) return draw;
    }
    return NULL;
}

const fake_draw_t *fakes__find_icon(const char *name) {
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        if (fakes.draws[i].kind == FAKE_ICON && strcmp(fakes.draws[i].text, name) == 0) return &fakes.draws[i];
    }
    return NULL;
}

void fakes__push(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value) {
    fakes.events[fakes.event_count++] = (bruce_input_event_t){
        .type = type,
        .action = action,
        .code = code,
        .value = value,
    };
}

void fakes__config_set(const char *key, const char *value) {
    for (size_t i = 0; i < FAKE_CONFIG_MAX; ++i) {
        if (!fakes.config[i].used || strcmp(fakes.config[i].key, key) == 0) {
            fakes.config[i].used = true;
            snprintf(fakes.config[i].key, sizeof(fakes.config[i].key), "%s", key);
            snprintf(fakes.config[i].value, sizeof(fakes.config[i].value), "%s", value);
            return;
        }
    }
}

const char *fakes__config_get(const char *key) {
    for (size_t i = 0; i < FAKE_CONFIG_MAX; ++i) {
        if (fakes.config[i].used && strcmp(fakes.config[i].key, key) == 0) return fakes.config[i].value;
    }
    return NULL;
}

image_bitmap_t fakes__bitmap(uint16_t size, uint16_t fill, bool ramp) {
    uint16_t *pixels = malloc((size_t)size * size * sizeof(uint16_t));
    for (size_t i = 0; i < (size_t)size * size; ++i) pixels[i] = (uint16_t)(ramp ? fill + i : fill);
    return (image_bitmap_t){.pixels = pixels, .width = size, .height = size};
}

static fake_draw_t *fakes__record(fake_draw_kind_t kind, int x, int y, int w, int h, uint16_t color) {
    if (fakes.draw_count >= FAKE_DRAWS_MAX) return NULL;
    fake_draw_t *draw = &fakes.draws[fakes.draw_count++];
    *draw = (fake_draw_t){.kind = kind, .x = x, .y = y, .w = w, .h = h, .color = color};
    return draw;
}

/* ---- display ------------------------------------------------------------ */

int display__width(void) { return fakes.width; }
int display__height(void) { return fakes.height; }
int display__screen_width(void) { return fakes.width; }
int display__screen_height(void) { return fakes.height; }
bool display__viewport_changed(void) { return false; }
bool display__await_redraw(void) { return false; }

bruce_result_t display__begin_frame(void) { return BRUCE_OK; }

bruce_result_t display__present(void) {
    fakes.frames++;
    return BRUCE_OK;
}

bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    fakes__record(FAKE_FILL_RECT, x, y, w, h, color);
    return BRUCE_OK;
}

bruce_result_t
display__fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color) {
    (void)r;
    fakes__record(FAKE_ROUND_RECT, x, y, w, h, color);
    return BRUCE_OK;
}

bruce_result_t display__draw_rgb_bitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    fake_draw_t *draw = fakes__record(FAKE_RGB_BITMAP, x, y, w, h, 0);
    if (draw != NULL) {
        draw->pixels = bitmap;
        memcpy(draw->first_pixels, bitmap, sizeof(draw->first_pixels));
    }
    return BRUCE_OK;
}

bruce_result_t display__draw_string(const char *text, int16_t x, int16_t y) {
    fake_draw_t *draw = fakes__record(FAKE_STRING, x, y, 0, 0, 0);
    if (draw != NULL) snprintf(draw->text, sizeof(draw->text), "%s", text);
    return BRUCE_OK;
}

/* Each icon's bits point at its own name, so a draw can be traced back to it. */
const bruce_icon_t *icon__get(const char *name) {
    static char names[16][32];
    static bruce_icon_t icons[16];
    static size_t next;
    size_t slot = next++ % 16;
    snprintf(names[slot], sizeof(names[slot]), "%s", name);
    icons[slot] = (bruce_icon_t){.width = 24, .height = 24, .bits = (const uint8_t *)names[slot]};
    return &icons[slot];
}

bruce_result_t display__draw_bitmap_scaled(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, int16_t dw, int16_t dh,
    bruce_display_color_t color
) {
    (void)w, (void)h, (void)dh;
    fake_draw_t *draw = fakes__record(FAKE_ICON, x, y, dw, dh, color);
    if (draw != NULL) snprintf(draw->text, sizeof(draw->text), "%s", (const char *)bitmap);
    return BRUCE_OK;
}

bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color) {
    (void)x, (void)y, (void)r, (void)color;
    return BRUCE_OK;
}

bruce_result_t display__set_text_color(bruce_display_color_t color) {
    (void)color;
    return BRUCE_OK;
}

bruce_result_t display__set_text_bg_color(uint32_t color) {
    (void)color;
    return BRUCE_OK;
}

bruce_result_t display__set_text_size(float size) {
    (void)size;
    return BRUCE_OK;
}

bruce_result_t display__get_font_metrics(int16_t *out_char_width, int16_t *out_char_height) {
    *out_char_width = 6;
    *out_char_height = 8;
    return BRUCE_OK;
}

/* ---- storage ------------------------------------------------------------ */

bruce_result_t storage__exists(const char *path, bool *out_exists) {
    *out_exists = fakes__file_exists(path);
    return BRUCE_OK;
}

bruce_result_t storage__mkdir(const char *path) {
    (void)path;
    fakes.mkdir_count++;
    return BRUCE_OK;
}

bruce_result_t storage__open(const char *path, uint32_t flags, bruce_file_id_t *out_file) {
    if (fakes__file(path) == NULL) {
        if (!(flags & BRUCE_STORAGE_OPEN_CREATE)) return BRUCE_ERR_NOT_FOUND;
        fakes__add_file(path, 0);
    }
    fake_file_t *file = fakes__file(path);
    if (flags & BRUCE_STORAGE_OPEN_TRUNCATE) file->size = 0;
    *out_file = (bruce_file_id_t)(file - fakes.files) + 1;
    return BRUCE_OK;
}

bruce_result_t storage__write(bruce_file_id_t file, const void *buffer, size_t size, size_t *out_size) {
    (void)buffer;
    fakes.files[file - 1].size += size;
    *out_size = size;
    return BRUCE_OK;
}

bruce_result_t storage__close(bruce_file_id_t file) {
    (void)file;
    return BRUCE_OK;
}

bruce_result_t storage__rename(const char *from, const char *to) {
    fake_file_t *file = fakes__file(from);
    if (file == NULL) return BRUCE_ERR_NOT_FOUND;
    snprintf(file->path, sizeof(file->path), "%s", to);
    return BRUCE_OK;
}

bruce_result_t storage__remove(const char *path) {
    fake_file_t *file = fakes__file(path);
    if (file == NULL) return BRUCE_ERR_NOT_FOUND;
    file->used = false;
    return BRUCE_OK;
}

/* ---- image -------------------------------------------------------------- */

bruce_result_t image__get_bitmap_from_file(
    const char *path, const bruce_image_draw_options_t *options, image_bitmap_t *out_bitmap
) {
    (void)options;
    fakes.decode_count++;
    if (!fakes__file_exists(path)) return BRUCE_ERR_NOT_FOUND;
    *out_bitmap = fakes__bitmap(fakes.decode_size, (uint16_t)fakes.decode_count, false);
    return BRUCE_OK;
}

void image__bitmap_release(image_bitmap_t *bitmap) {
    free(bitmap->pixels);
    memset(bitmap, 0, sizeof(*bitmap));
}

/* ---- network ------------------------------------------------------------ */

bool wifi__is_connected(void) { return fakes.wifi_connected; }

bruce_result_t wifi__connect_known(void) {
    fakes.wifi_connect_calls++;
    fakes.wifi_connected = fakes.wifi_connect_ok;
    return fakes.wifi_connect_ok ? BRUCE_OK : BRUCE_ERR_IO;
}

int fakes__http_active(void) {
    int active = 0;
    for (size_t i = 0; i < FAKE_JOBS_MAX; ++i) active += fakes.http_jobs[i].used;
    return active;
}

void fakes__http_finish_all(void) {
    for (size_t i = 0; i < FAKE_JOBS_MAX; ++i) fakes.http_jobs[i].done = true;
}

static int fakes__http_job(bruce_http_job_id_t id) {
    for (int i = 0; i < FAKE_JOBS_MAX; ++i) {
        if (fakes.http_jobs[i].used && fakes.http_jobs[i].id == id) return i;
    }
    return -1;
}

bruce_result_t http__request_start(const bruce_http_request_t *request, bruce_http_job_id_t *out_job) {
    if (fakes.http_start_result != BRUCE_OK) return fakes.http_start_result;
    int slot = -1;
    for (int i = 0; i < FAKE_JOBS_MAX && slot < 0; ++i) slot = fakes.http_jobs[i].used ? -1 : i;
    if (slot < 0) return BRUCE_ERR_RESOURCE_LIMIT;
    fakes.http_calls++;
    snprintf(fakes.http_url, sizeof(fakes.http_url), "%s", request->url);
    for (size_t i = 0; i < request->header_count; ++i) {
        if (strcmp(request->headers[2 * i], "User-Agent") == 0) {
            snprintf(fakes.http_user_agent, sizeof(fakes.http_user_agent), "%s", request->headers[2 * i + 1]);
        }
    }
    fakes.http_jobs[slot].used = true;
    fakes.http_jobs[slot].done = !fakes.http_manual;
    fakes.http_jobs[slot].id = (bruce_http_job_id_t)fakes.http_calls;
    *out_job = fakes.http_jobs[slot].id;
    int active = fakes__http_active();
    if (active > fakes.http_max_active) fakes.http_max_active = active;
    return BRUCE_OK;
}

bruce_result_t http__request_poll(bruce_http_job_id_t job, bruce_http_response_t *response) {
    memset(response, 0, sizeof(*response));
    int slot = fakes__http_job(job);
    if (slot < 0) return BRUCE_ERR_NOT_FOUND;
    if (!fakes.http_jobs[slot].done) return BRUCE_ERR_BUSY;
    fakes.http_jobs[slot].used = false;
    if (fakes.http_result != BRUCE_OK) return fakes.http_result;
    response->status_code = fakes.http_status;
    response->body = calloc(1, fakes.http_body_len + 1);
    response->body_len = fakes.http_body_len;
    return BRUCE_OK;
}

void http__request_cancel(bruce_http_job_id_t job) {
    int slot = fakes__http_job(job);
    if (slot < 0) return;
    fakes.http_jobs[slot].used = false;
    fakes.http_cancels++;
}

void http__response_free(bruce_http_response_t *response) {
    free(response->body);
    memset(response, 0, sizeof(*response));
}

/* ---- memory ------------------------------------------------------------- */

void *memory__calloc(size_t count, size_t size) { return calloc(count, size); }
void memory__free(void *ptr) { free(ptr); }

bruce_result_t memory__get_stats(bruce_memory_stats_t *out_stats) {
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->psram_free = fakes.psram_free;
    return BRUCE_OK;
}

/* ---- app_config --------------------------------------------------------- */

int app_config__get_int(const char *app_name, const char *json_path, int default_value) {
    (void)app_name;
    const char *value = fakes__config_get(json_path);
    return value != NULL ? atoi(value) : default_value;
}

bruce_result_t app_config__set_int(const char *app_name, const char *json_path, int value) {
    (void)app_name;
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    fakes__config_set(json_path, text);
    fakes.config_writes++;
    return BRUCE_OK;
}

bool app_config__get_bool(const char *app_name, const char *json_path, bool default_value) {
    (void)app_name;
    const char *value = fakes__config_get(json_path);
    return value != NULL ? strcmp(value, "true") == 0 : default_value;
}

bruce_result_t app_config__set_bool(const char *app_name, const char *json_path, bool value) {
    (void)app_name;
    fakes__config_set(json_path, value ? "true" : "false");
    fakes.config_writes++;
    return BRUCE_OK;
}

bool app_config__get_string(
    const char *app_name, const char *json_path, const char *default_value, char *out_value, size_t capacity
) {
    (void)app_name;
    const char *value = fakes__config_get(json_path);
    snprintf(out_value, capacity, "%s", value != NULL ? value : default_value);
    return value != NULL;
}

/* ---- input, dialogs, runtime -------------------------------------------- */

bruce_result_t input__read(bruce_input_event_t *out_event, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (fakes.event_next < fakes.event_count) {
        *out_event = fakes.events[fakes.event_next++];
        return BRUCE_OK;
    }
    if (fakes.polls_until_back > 0 && --fakes.polls_until_back == 0) {
        *out_event = (bruce_input_event_t){
            .type = BRUCE_INPUT_BUTTON,
            .action = BRUCE_INPUT_PRESS,
            .code = BRUCE_INPUT_CODE_BACK,
        };
        return BRUCE_OK;
    }
    return BRUCE_ERR_TIMEOUT;
}

bool input__wait_long_press(const bruce_input_event_t *press, uint32_t long_ms) {
    (void)press, (void)long_ms;
    return fakes.long_press;
}

uint64_t runtime__now(void) { return fakes.now_ms; }

bruce_result_t dialog__text_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size
) {
    (void)title, (void)prompt, (void)initial_text, (void)mask_input;
    if (fakes.text_input == NULL) return BRUCE_ERR_CANCELLED;
    snprintf(buffer, buffer_size, "%s", fakes.text_input);
    return BRUCE_OK;
}

bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    (void)kind, (void)title, (void)message;
    fakes.message_count++;
    return BRUCE_OK;
}

bruce_result_t dialog__choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title, (void)message;
    fakes.choice_calls++;
    fakes.choice_count = choice_count;
    for (size_t i = 0; i < choice_count && i < 8; ++i) {
        const char *right = choices[i].right_text != NULL ? choices[i].right_text : "";
        const char *icon = choices[i].icon_name != NULL ? choices[i].icon_name : "";
        snprintf(fakes.choice_labels[i], sizeof(fakes.choice_labels[i]), "%s", choices[i].label);
        snprintf(fakes.choice_icons[i], sizeof(fakes.choice_icons[i]), "%s", icon);
        snprintf(fakes.choice_right_text[i], sizeof(fakes.choice_right_text[i]), "%s", right);
    }
    *out_selected = fakes.choice;
    return fakes.choice_result;
}

bruce_result_t runtime__delay(uint32_t milliseconds) {
    (void)milliseconds;
    return BRUCE_OK;
}
