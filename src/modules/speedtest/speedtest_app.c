#include "speedtest_app.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/config.h"
#include "core_sdk/http.h"
#include "core_sdk/input.h"
#include "core_sdk/lvgl.h"
#include "core_sdk/memory.h"
#include "core_sdk/net.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/wifi.h"

/* Cloudflare's speed test endpoints. Plain HTTP, so the ESP's TLS work
 * doesn't limit the result. */
#define SPEED_DOWN_URL "http://speed.cloudflare.com/__down?bytes="
#define SPEED_UP_URL "http://speed.cloudflare.com/__up"
#define SPEED_USER_AGENT "BruceOS-SpeedTest/0.1"

#define SPEED_PINGS 5
#define SPEED_DOWN_BYTES 25000000u /* Cloudflare's largest */
#define SPEED_DOWN_MS 8000u
#define SPEED_UP_FIRST 65536u
#define SPEED_UP_MAX 1048576u
#define SPEED_UP_ROUND_MS 2000u /* A round this long is measured enough. */
#define SPEED_UP_TOTAL_MS 10000u
#define SPEED_TIMEOUT_MS 20000u
#define SPEED_READ_BYTES 16384u
#define SPEED_TEST_WAIT_MS 10u
#define SPEED_IDLE_WAIT_MS 100u
#define SPEED_REDRAW_MS 250u

typedef enum {
    PHASE_IDLE,
    PHASE_CONNECT,
    PHASE_LATENCY,
    PHASE_DOWNLOAD,
    PHASE_UPLOAD,
    PHASE_DONE,
    PHASE_FAILED,
} phase_t;

typedef struct {
    lv_obj_t *value;
    lv_obj_t *caption;
} result_box_t;

typedef struct {
    phase_t phase;
    const char *failure;
    bool running;
    bool restart;
    bool wifi; /* The link in use when the test began. */
    /* One request at a time. */
    bruce_http_job_id_t job;
    bool job_active;
    uint64_t job_started;
    /* Latency */
    int pings;
    uint32_t latency_ms;
    /* Download */
    uint64_t first_byte_at;
    uint64_t down_bytes;
    double down_mbps;
    /* Upload */
    char *up_body;
    size_t up_capacity;
    size_t up_size;
    uint64_t up_started;
    double up_mbps;
    double live_mbps;
    uint8_t read_buffer[SPEED_READ_BYTES];
    /* Widgets */
    lv_obj_t *link;
    lv_obj_t *live;
    lv_obj_t *status;
    lv_obj_t *progress;
    result_box_t latency;
    result_box_t download;
    result_box_t upload;
    uint64_t drawn_at;
} speed_t;

static const char *const s_headers[] = {"User-Agent", SPEED_USER_AGENT, "Content-Type", "application/octet-stream"};

static double speed__mbps(uint64_t bytes, uint64_t ms) { return ms == 0 ? 0 : (double)bytes * 8.0 / (double)ms / 1000.0; }

static void speed__format_mbps(double mbps, char *out, size_t capacity) {
    if (mbps < 1) snprintf(out, capacity, "%.2f", mbps);
    else if (mbps < 100) snprintf(out, capacity, "%.1f", mbps);
    else snprintf(out, capacity, "%.0f", mbps);
}

/* ---- Requests ----------------------------------------------------------------- */

static void speed__stop_job(speed_t *speed) {
    if (speed->job_active) http__request_cancel(speed->job);
    speed->job_active = false;
}

static bool speed__start_job(speed_t *speed, const char *url, const char *method, const char *body, size_t body_len,
                             bool stream) {
    bruce_http_request_t request = {
        .url = url,
        .method = method,
        .body = body,
        .body_len = body_len,
        .headers = s_headers,
        .header_count = body != NULL ? 2 : 1,
        .timeout_ms = SPEED_TIMEOUT_MS,
        .max_response_bytes = stream ? SPEED_DOWN_BYTES + 1u : 0,
        .stream = stream,
    };
    speed->job_active = http__request_start(&request, &speed->job) == BRUCE_OK;
    speed->job_started = runtime__now();
    return speed->job_active;
}

/* BRUCE_ERR_BUSY while running, else the result (BRUCE_ERR_IO for a bad status). */
static bruce_result_t speed__poll_job(speed_t *speed) {
    bruce_http_response_t response;
    bruce_result_t result = http__request_poll(speed->job, &response);
    if (result == BRUCE_ERR_BUSY) return result;
    speed->job_active = false;
    if (result == BRUCE_OK) {
        if (response.status_code != 200) result = BRUCE_ERR_IO;
        http__response_free(&response);
    }
    return result;
}

static void speed__fail(speed_t *speed, const char *failure) {
    speed__stop_job(speed);
    speed->phase = PHASE_FAILED;
    speed->failure = failure;
}

/* ---- Phases ------------------------------------------------------------------ */

static void speed__start_latency(speed_t *speed) {
    speed->wifi = wifi__is_connected();
    speed->phase = PHASE_LATENCY;
    speed->pings = 0;
    speed->latency_ms = 0;
}

static void speed__start_download(speed_t *speed) {
    char url[96];
    snprintf(url, sizeof(url), "%s%u", SPEED_DOWN_URL, SPEED_DOWN_BYTES);
    speed->phase = PHASE_DOWNLOAD;
    speed->first_byte_at = 0;
    speed->down_bytes = 0;
    speed->live_mbps = 0;
    if (!speed__start_job(speed, url, "GET", NULL, 0, true)) speed__fail(speed, "Download failed");
}

static void speed__start_upload_round(speed_t *speed) {
    if (!speed__start_job(speed, SPEED_UP_URL, "POST", speed->up_body, speed->up_size, false)) {
        speed__fail(speed, "Upload failed");
    }
}

static void speed__start_upload(speed_t *speed) {
    speed->phase = PHASE_UPLOAD;
    speed->live_mbps = 0;
    if (speed->up_body == NULL) {
        for (size_t size = SPEED_UP_MAX; size >= SPEED_UP_FIRST && speed->up_body == NULL; size /= 4) {
            speed->up_body = memory__malloc(size);
            if (speed->up_body != NULL) speed->up_capacity = size;
        }
        if (speed->up_body == NULL) {
            speed__fail(speed, "Out of memory");
            return;
        }
        for (size_t i = 0; i < speed->up_capacity; ++i) speed->up_body[i] = (char)('a' + i % 26);
    }
    speed->up_size = SPEED_UP_FIRST;
    speed->up_started = runtime__now();
    speed__start_upload_round(speed);
}

static void speed__start(speed_t *speed) {
    speed__stop_job(speed);
    speed->down_mbps = 0;
    speed->up_mbps = 0;
    speed->live_mbps = 0;
    speed->latency_ms = 0;
    if (net__is_online()) {
        speed__start_latency(speed);
    } else if (net__connect_start() == BRUCE_OK) {
        speed->phase = PHASE_CONNECT;
    } else {
        speed__fail(speed, "No internet connection");
    }
}

/* Smallest of a few tiny requests: one TCP connect plus one request, so
 * about two round trips to Cloudflare. */
static void speed__poll_latency(speed_t *speed) {
    if (!speed->job_active) {
        char url[96];
        snprintf(url, sizeof(url), "%s0", SPEED_DOWN_URL);
        if (!speed__start_job(speed, url, "GET", NULL, 0, false)) speed__fail(speed, "Server not reachable");
        return;
    }
    bruce_result_t result = speed__poll_job(speed);
    if (result == BRUCE_ERR_BUSY) return;
    if (result != BRUCE_OK) {
        speed__fail(speed, "Server not reachable");
        return;
    }
    uint32_t took = (uint32_t)(runtime__now() - speed->job_started);
    if (speed->pings == 0 || took < speed->latency_ms) speed->latency_ms = took;
    if (++speed->pings == SPEED_PINGS) speed__start_download(speed);
}

/* Counted from the first byte, so connecting doesn't count. */
static void speed__poll_download(speed_t *speed) {
    bool finished = false;
    for (;;) {
        size_t length = 0;
        int status = 0;
        bruce_result_t result =
            http__request_read(speed->job, speed->read_buffer, sizeof(speed->read_buffer), &length, &status);
        if (result == BRUCE_ERR_BUSY) break;
        if (result != BRUCE_OK || length == 0 || (status != 0 && status != 200)) {
            speed->job_active = result == BRUCE_OK && length > 0;
            if (result != BRUCE_OK && speed->down_bytes == 0) {
                speed__fail(speed, "Download failed");
                return;
            }
            finished = true;
            break;
        }
        if (speed->first_byte_at == 0) speed->first_byte_at = runtime__now();
        speed->down_bytes += length;
    }
    if (speed->first_byte_at == 0) return;
    uint64_t elapsed = runtime__now() - speed->first_byte_at;
    speed->live_mbps = speed__mbps(speed->down_bytes, elapsed);
    if (finished || elapsed >= SPEED_DOWN_MS) {
        speed__stop_job(speed);
        speed->down_mbps = speed->live_mbps;
        speed__start_upload(speed);
    }
}

/* Growing rounds until one is long enough to measure. The upload time is the
 * whole request less the latency request's time (connecting and replying). */
static void speed__poll_upload(speed_t *speed) {
    bruce_result_t result = speed__poll_job(speed);
    if (result == BRUCE_ERR_BUSY) return;
    if (result != BRUCE_OK) {
        if (speed->up_mbps > 0) speed->phase = PHASE_DONE;
        else speed__fail(speed, "Upload failed");
        return;
    }
    uint64_t now = runtime__now();
    uint64_t took = now - speed->job_started;
    uint64_t sending = took > speed->latency_ms ? took - speed->latency_ms : 1;
    speed->up_mbps = speed__mbps(speed->up_size, sending);
    speed->live_mbps = speed->up_mbps;
    bool long_enough = took >= SPEED_UP_ROUND_MS || now - speed->up_started >= SPEED_UP_TOTAL_MS;
    if (long_enough || speed->up_size * 4 > speed->up_capacity) {
        speed->phase = PHASE_DONE;
        return;
    }
    speed->up_size *= 4;
    speed__start_upload_round(speed);
}

static void speed__poll(speed_t *speed) {
    switch (speed->phase) {
        case PHASE_CONNECT: {
            bruce_result_t result = net__connect_poll();
            if (result == BRUCE_OK) speed__start_latency(speed);
            else if (result != BRUCE_ERR_BUSY) speed__fail(speed, "No internet connection");
            break;
        }
        case PHASE_LATENCY: speed__poll_latency(speed); break;
        case PHASE_DOWNLOAD: speed__poll_download(speed); break;
        case PHASE_UPLOAD: speed__poll_upload(speed); break;
        default: break;
    }
}

static bool speed__testing(const speed_t *speed) {
    return speed->phase != PHASE_IDLE && speed->phase != PHASE_DONE && speed->phase != PHASE_FAILED;
}

/* ---- Screen ------------------------------------------------------------------ */

static lv_obj_t *speed__box(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_flex_flow(box, flow);
    return box;
}

static lv_obj_t *speed__label(lv_obj_t *parent, int scale, uint16_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lvgl__font(scale), 0);
    lv_obj_set_style_text_color(label, lvgl__color(color), 0);
    lv_label_set_text(label, "");
    return label;
}

static void speed__result_box(lv_obj_t *parent, result_box_t *box, const char *caption) {
    lv_obj_t *column = speed__box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(column, 1);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    box->caption = speed__label(column, 1, config__get_color_text_muted());
    lv_label_set_text(box->caption, caption);
    box->value = speed__label(column, 2, config__get_color_text());
    lv_label_set_text(box->value, "-");
}

static void speed__clicked(lv_event_t *event) {
    speed_t *speed = lv_event_get_user_data(event);
    speed->restart = true;
}

static void speed__build(speed_t *speed) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(screen, 4, 0);
    lv_obj_set_style_pad_gap(screen, 4, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    speed->link = speed__label(screen, 1, config__get_color_text_muted());
    lv_obj_t *middle = speed__box(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(middle, LV_PCT(100));
    lv_obj_set_flex_grow(middle, 1);
    lv_obj_set_flex_align(middle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    speed->live = speed__label(middle, 4, config__get_color_primary());
    speed->status = speed__label(middle, 1, config__get_color_text_muted());
    speed->progress = lv_bar_create(screen);
    lv_obj_set_size(speed->progress, LV_PCT(100), 4);
    lv_bar_set_range(speed->progress, 0, 100);

    lv_obj_t *results = speed__box(screen, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(results, LV_PCT(100));
    lv_obj_set_height(results, LV_SIZE_CONTENT);
    speed__result_box(results, &speed->latency, "Latency ms");
    speed__result_box(results, &speed->download, "Down Mbit/s");
    speed__result_box(results, &speed->upload, "Up Mbit/s");

    lv_obj_t *again = lv_button_create(screen);
    lv_obj_set_width(again, LV_PCT(100));
    lv_obj_set_flex_flow(again, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(again, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(again, 6, 0);
    lvgl__icon_create(again, "replay", 16);
    lv_label_set_text(lv_label_create(again), "Test again");
    lv_obj_add_event_cb(again, speed__clicked, LV_EVENT_CLICKED, speed);
}

static void speed__set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static int speed__progress(const speed_t *speed) {
    uint64_t now = runtime__now();
    switch (speed->phase) {
        case PHASE_LATENCY: return speed->pings * 10 / SPEED_PINGS;
        case PHASE_DOWNLOAD:
            if (speed->first_byte_at == 0) return 10;
            return 10 + (int)((now - speed->first_byte_at) * 50 / SPEED_DOWN_MS);
        case PHASE_UPLOAD: return 60 + (int)((now - speed->up_started) * 40 / SPEED_UP_TOTAL_MS);
        case PHASE_DONE: return 100;
        default: return 0;
    }
}

static void speed__update(speed_t *speed) {
    char text[48];
    bool online = speed__testing(speed) || speed->phase == PHASE_DONE ? true : net__is_online();
    const char *link = !online ? "Not connected" : (speed->phase == PHASE_IDLE ? wifi__is_connected() : speed->wifi)
                                                       ? "Wi-Fi"
                                                       : "Mobile data";
    snprintf(text, sizeof(text), "Speed test - %s", link);
    speed__set_text(speed->link, text);

    if (speed->phase == PHASE_LATENCY && speed->pings > 0) snprintf(text, sizeof(text), "%u ms", (unsigned)speed->latency_ms);
    else if (speed->phase == PHASE_DOWNLOAD || speed->phase == PHASE_UPLOAD || speed->phase == PHASE_DONE) {
        speed__format_mbps(speed->phase == PHASE_DONE ? speed->down_mbps : speed->live_mbps, text, sizeof(text));
    } else snprintf(text, sizeof(text), "-");
    speed__set_text(speed->live, text);

    const char *status = speed->phase == PHASE_CONNECT    ? "Connecting..."
                         : speed->phase == PHASE_LATENCY  ? "Measuring latency..."
                         : speed->phase == PHASE_DOWNLOAD ? "Download Mbit/s"
                         : speed->phase == PHASE_UPLOAD   ? "Upload Mbit/s"
                         : speed->phase == PHASE_DONE     ? "Download Mbit/s - done"
                         : speed->phase == PHASE_FAILED   ? speed->failure
                                                          : "";
    speed__set_text(speed->status, status);
    int progress = speed__progress(speed);
    if (progress > 100) progress = 100;
    if (lv_bar_get_value(speed->progress) != progress) lv_bar_set_value(speed->progress, progress, LV_ANIM_OFF);

    if (speed->latency_ms > 0 && speed->phase != PHASE_LATENCY) snprintf(text, sizeof(text), "%u", (unsigned)speed->latency_ms);
    else snprintf(text, sizeof(text), "-");
    speed__set_text(speed->latency.value, text);
    if (speed->down_mbps > 0) speed__format_mbps(speed->down_mbps, text, sizeof(text));
    else snprintf(text, sizeof(text), "-");
    speed__set_text(speed->download.value, text);
    if (speed->up_mbps > 0 && speed->phase != PHASE_UPLOAD) speed__format_mbps(speed->up_mbps, text, sizeof(text));
    else snprintf(text, sizeof(text), "-");
    speed__set_text(speed->upload.value, text);
}

/* ---- App ---------------------------------------------------------------------- */

static void speed__on_input(speed_t *speed, const bruce_input_event_t *event) {
    bool back = event->type != BRUCE_INPUT_TOUCH && event->action == BRUCE_INPUT_PRESS &&
                event->code == BRUCE_INPUT_CODE_BACK;
    if (back) speed->running = false;
    else lvgl__input(event);
}

int speedtest_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    speed_t *speed = memory__calloc(1, sizeof(*speed));
    if (speed == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t started = lvgl__begin();
    if (started != BRUCE_OK) {
        memory__free(speed);
        return started;
    }
    speed__build(speed);
    speed->running = true;
    speed->restart = true;
    while (speed->running) {
        if (speed->restart) {
            speed->restart = false;
            speed__start(speed);
        }
        speed__poll(speed);
        /* Drawing costs time the download needs, so only now and then while testing. */
        uint64_t now = runtime__now();
        bool testing = speed__testing(speed);
        if (!testing || now - speed->drawn_at >= SPEED_REDRAW_MS) {
            speed->drawn_at = now;
            speed__update(speed);
        }
        uint32_t wait = lvgl__step();
        uint32_t most = testing ? SPEED_TEST_WAIT_MS : SPEED_IDLE_WAIT_MS;
        bruce_input_event_t event;
        bruce_result_t result = input__read(&event, wait < most ? wait : most);
        if (result == BRUCE_OK) speed__on_input(speed, &event);
        else if (result == BRUCE_ERR_NOT_FOREGROUND) (void)runtime__delay(most);
        else if (result != BRUCE_ERR_TIMEOUT) break;
    }
    speed__stop_job(speed);
    lvgl__end();
    memory__free(speed->up_body);
    memory__free(speed);
    return BRUCE_OK;
}
