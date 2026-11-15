#pragma once

/* Host fakes for the Bruce SDK calls the maps app makes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/image.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"

#define FAKE_DRAWS_MAX 2048
#define FAKE_FILES_MAX 128
#define FAKE_CONFIG_MAX 16
#define FAKE_EVENTS_MAX 16

typedef enum {
    FAKE_FILL_RECT,
    FAKE_RGB_BITMAP,
    FAKE_ROUND_RECT,
    FAKE_STRING,
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

    int http_calls;
    char http_url[256];
    char http_user_agent[64];
    bruce_result_t http_result;
    int http_status;
    size_t http_body_len;

    bool wifi_connected;
    bool wifi_connect_ok;
    int wifi_connect_calls;

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
    const char *text_input;
    size_t choice;
    bruce_result_t choice_result;
    int choice_calls;
    char choice_right_text[2][8];
    int message_count;
} fakes_t;

extern fakes_t fakes;

void fakes__reset(void);
void fakes__add_file(const char *path, size_t size);
bool fakes__file_exists(const char *path);
size_t fakes__count(fake_draw_kind_t kind);
const fake_draw_t *fakes__find_string(const char *text);
const fake_draw_t *fakes__find_prefix(const char *prefix);
void fakes__push(bruce_input_type_t type, bruce_input_action_t action, int32_t code, int32_t value);
void fakes__config_set(const char *key, const char *value);
const char *fakes__config_get(const char *key);

/* Heap bitmap of `size` x `size` pixels, pixel i = fill + i when `ramp`, else fill. */
image_bitmap_t fakes__bitmap(uint16_t size, uint16_t fill, bool ramp);
