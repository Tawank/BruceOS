#pragma once

/* Host fakes for the Bruce SDK calls the maps app makes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/http.h"
#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"

#define FAKE_DRAWS_MAX 2048
#define FAKE_FILES_MAX 128
#define FAKE_CONFIG_MAX 16
#define FAKE_EVENTS_MAX 16
#define FAKE_JOBS_MAX 32

typedef enum {
    FAKE_FILL_RECT,
    FAKE_RGB_BITMAP,
    FAKE_ROUND_RECT,
    FAKE_STRING,
    FAKE_ICON, /* text holds the icon name */
} fake_draw_kind_t;

typedef struct {
    fake_draw_kind_t kind;
    int x, y, w, h;
    uint16_t color;
    const uint16_t *pixels;
    uint16_t first_pixels[4];
    char text[48];
} fake_draw_t;

typedef struct {
    bool used;
    char path[192];
    size_t size;
} fake_file_t;

typedef struct {
    int width, height;
    fake_draw_t draws[FAKE_DRAWS_MAX];
    size_t draw_count;
    int frames;

    fake_file_t files[FAKE_FILES_MAX];
    int mkdir_count;

    int decode_count;
    uint16_t decode_size;

    int http_calls; /* Background requests started. */
    char http_url[256];
    char http_user_agent[64];
    bruce_result_t http_start_result;
    bruce_result_t http_result;
    int http_status;
    size_t http_body_len;
    const char *http_body; /* Response body; zeros of http_body_len when NULL. */
    bool http_manual; /* Jobs stay busy until fakes__http_finish_all(). */
    struct {
        bool used;
        bool done;
        bruce_http_job_id_t id;
    } http_jobs[FAKE_JOBS_MAX];
    int http_cancels;
    int http_max_active;

    bool wifi_connected;
    bool has_keyboard;
    bool has_touch;
    bool epaper;
    bool wifi_connect_ok;
    int wifi_connect_calls;  /* Background connects started. */
    bool wifi_connect_manual; /* Connect stays busy until fakes__wifi_finish(). */
    bool wifi_connect_busy;

    size_t psram_free;

    struct {
        bool used;
        char key[32];
        char value[128];
    } config[FAKE_CONFIG_MAX];
    int config_writes;

    bruce_input_event_t events[FAKE_EVENTS_MAX];
    size_t event_count, event_next;
    int polls_until_back; /* >0: Back arrives after this many empty polls. */
    bool long_press;
    uint64_t now_ms;
    const char *text_input;
    size_t choice;
    bruce_result_t choice_result;
    int choice_calls;
    size_t choice_count;
    char choice_labels[8][32];
    char choice_icons[8][32];
    char choice_right_text[8][8];
    int message_count;
    char message[64];
} fakes_t;

extern fakes_t fakes;

void fakes__reset(void);
void fakes__add_file(const char *path, size_t size);
bool fakes__file_exists(const char *path);
size_t fakes__count(fake_draw_kind_t kind);
const fake_draw_t *fakes__find_string(const char *text);
const fake_draw_t *fakes__find_prefix(const char *prefix);
const fake_draw_t *fakes__find_icon(const char *name);
void fakes__push(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value);
void fakes__config_set(const char *key, const char *value);
void fakes__http_finish_all(void);
void fakes__wifi_finish(void);
int fakes__http_active(void);
const char *fakes__config_get(const char *key);

/* Heap bitmap of `size` x `size` pixels, pixel i = fill + i when `ramp`, else fill. */
image_bitmap_t fakes__bitmap(uint16_t size, uint16_t fill, bool ramp);
