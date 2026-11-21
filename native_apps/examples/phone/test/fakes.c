#include "fakes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/appdata.h"
#include "core_sdk/clock.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/icon.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"

fakes_t fakes;

void fakes__reset(void) {
    memset(&fakes, 0, sizeof(fakes));
    fakes.width = 240;
    fakes.height = 320;
    fakes.available = true;
    fakes.enabled = true;
    fakes.status.state = BRUCE_CELLULAR_READY;
    fakes.status.signal_bars = 3;
    fakes.status.volume = 60;
    snprintf(fakes.status.operator_name, sizeof(fakes.status.operator_name), "Orange");
    fakes.now_utc = 1763700000; /* 2025-11-21 04:40 UTC, a Friday */
}

static const fake_draw_t *fakes__find(const char *text, bool icon) {
    for (size_t i = 0; i < fakes.draw_count; ++i) {
        if (fakes.draws[i].icon == icon && strcmp(fakes.draws[i].text, text) == 0) return &fakes.draws[i];
    }
    return NULL;
}

const fake_draw_t *fakes__find_string(const char *text) { return fakes__find(text, false); }

const fake_draw_t *fakes__find_icon(const char *name) { return fakes__find(name, true); }

fake_file_t *fakes__file(const char *name) {
    for (size_t i = 0; i < FAKE_FILES_MAX; ++i) {
        if (fakes.files[i].used && strcmp(fakes.files[i].name, name) == 0) return &fakes.files[i];
    }
    return NULL;
}

void fakes__set_file(const char *name, const char *text) {
    fake_file_t *file = fakes__file(name);
    for (size_t i = 0; file == NULL && i < FAKE_FILES_MAX; ++i) {
        if (!fakes.files[i].used) file = &fakes.files[i];
    }
    file->used = true;
    snprintf(file->name, sizeof(file->name), "%s", name);
    snprintf(file->data, sizeof(file->data), "%s", text);
    file->size = strlen(file->data);
}

void fakes__log(
    bool incoming, bruce_call_result_t result, const char *number, int64_t utc, uint32_t seconds
) {
    bruce_cellular_call_record_t *record = &fakes.log[fakes.log_count++];
    memset(record, 0, sizeof(*record));
    record->sequence = (uint32_t)fakes.log_count;
    record->incoming = incoming;
    record->result = result;
    snprintf(record->number, sizeof(record->number), "%s", number);
    record->started_utc = utc;
    record->seconds = seconds;
    fakes.status.last_log_sequence = record->sequence;
}

/* ---- appdata, memory, clock, runtime ---- */

bruce_result_t appdata__read(const char *name, void *buffer, size_t capacity, size_t *out_size) {
    fake_file_t *file = fakes__file(name);
    *out_size = 0;
    if (file == NULL) return BRUCE_ERR_NOT_FOUND;
    size_t size = file->size < capacity ? file->size : capacity;
    memcpy(buffer, file->data, size);
    *out_size = size;
    return file->size > capacity ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_OK;
}

bruce_result_t appdata__write(const char *name, const void *data, size_t size) {
    char text[4096];
    if (size >= sizeof(text)) return BRUCE_ERR_RESOURCE_LIMIT;
    memcpy(text, data, size);
    text[size] = '\0';
    fakes__set_file(name, text);
    fakes.writes++;
    return BRUCE_OK;
}

void *memory__malloc(size_t size) { return malloc(size); }
void *memory__calloc(size_t count, size_t size) { return calloc(count, size); }
void memory__free(void *ptr) { free(ptr); }

bruce_result_t clock__get_utc(bruce_clock_datetime_t *out) {
    return clock__epoch_to_datetime(fakes.now_utc, out);
}

bruce_result_t clock__datetime_to_epoch(const bruce_clock_datetime_t *value, int64_t *out_epoch) {
    int y = value->year, m = value->month;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + value->day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    *out_epoch = days * 86400 + value->hour * 3600 + value->minute * 60 + value->second;
    return BRUCE_OK;
}

bruce_result_t clock__epoch_to_datetime(int64_t epoch, bruce_clock_datetime_t *out) {
    int64_t days = epoch / 86400, rem = epoch % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int day = (int)(doy - (153 * mp + 2) / 5 + 1);
    int month = (int)(mp < 10 ? mp + 3 : mp - 9);
    out->year = (uint16_t)(yoe + era * 400 + (month <= 2));
    out->month = (uint8_t)month;
    out->day = (uint8_t)day;
    out->hour = (uint8_t)(rem / 3600);
    out->minute = (uint8_t)(rem / 60 % 60);
    out->second = (uint8_t)(rem % 60);
    return BRUCE_OK;
}

int64_t clock__get_local_offset_seconds(void) { return 3600; }

uint64_t runtime__now(void) { return fakes.now_ms; }

bruce_result_t runtime__delay(uint32_t milliseconds) {
    fakes.now_ms += milliseconds;
    return BRUCE_OK;
}

const char *result__to_string(int result) { return result == BRUCE_OK ? "OK" : "Error"; }

/* ---- cellular ---- */

bool cellular__is_available(void) { return fakes.available; }

bruce_result_t cellular__set_enabled(bool enabled) {
    fakes.enabled = enabled;
    fakes.enables++;
    fakes.status.state = enabled ? BRUCE_CELLULAR_STARTING : BRUCE_CELLULAR_OFF;
    return BRUCE_OK;
}

bruce_result_t cellular__get_status(bruce_cellular_status_t *out) {
    *out = fakes.status;
    return BRUCE_OK;
}

bruce_result_t cellular__dial(const char *number) {
    snprintf(fakes.dialed, sizeof(fakes.dialed), "%s", number);
    if (fakes.dial_result != BRUCE_OK) return fakes.dial_result;
    fakes.status.call = BRUCE_CALL_DIALING;
    snprintf(fakes.status.call_number, sizeof(fakes.status.call_number), "%s", number);
    return BRUCE_OK;
}

bruce_result_t cellular__answer(void) {
    fakes.answers++;
    fakes.status.call = BRUCE_CALL_ACTIVE;
    return BRUCE_OK;
}

bruce_result_t cellular__hang_up(void) {
    fakes.hang_ups++;
    fakes.status.call = BRUCE_CALL_NONE;
    return BRUCE_OK;
}

bruce_result_t cellular__send_dtmf(char digit) {
    size_t length = strlen(fakes.dtmf);
    fakes.dtmf[length] = digit;
    fakes.dtmf[length + 1] = '\0';
    return BRUCE_OK;
}

bruce_result_t cellular__set_muted(bool muted) {
    fakes.status.muted = muted;
    return BRUCE_OK;
}

bruce_result_t cellular__set_volume(uint8_t volume) {
    fakes.status.volume = volume;
    return BRUCE_OK;
}

bruce_result_t cellular__ussd(const char *code, char *out_reply, size_t capacity, uint32_t timeout_ms) {
    (void)timeout_ms;
    snprintf(fakes.ussd_code, sizeof(fakes.ussd_code), "%s", code);
    if (fakes.ussd_reply == NULL) return BRUCE_ERR_TIMEOUT;
    snprintf(out_reply, capacity, "%s", fakes.ussd_reply);
    return BRUCE_OK;
}

size_t cellular__get_call_log(uint32_t after_sequence, bruce_cellular_call_record_t *out, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < fakes.log_count && count < capacity; ++i) {
        if (fakes.log[i].sequence > after_sequence) out[count++] = fakes.log[i];
    }
    return count;
}

/* ---- display, icons, input, dialogs ---- */

static void fakes__record(bool icon, const char *text, int x, int y, uint16_t color) {
    if (fakes.draw_count >= FAKE_DRAWS_MAX) return;
    fake_draw_t *draw = &fakes.draws[fakes.draw_count++];
    draw->icon = icon;
    draw->x = x;
    draw->y = y;
    draw->color = color;
    snprintf(draw->text, sizeof(draw->text), "%s", text);
}

static uint16_t s_text_color;

int display__width(void) { return fakes.width; }
int display__height(void) { return fakes.height; }
bool display__is_epaper(void) { return fakes.epaper; }
bool display__viewport_changed(void) { return false; }
bool display__await_redraw(void) { return false; }
bool process__is_background(void) { return false; }
bruce_result_t dialog__draw_window(bruce_dialog_window_area_t *out_area) {
    *out_area = (bruce_dialog_window_area_t){.width = fakes.width, .height = fakes.height};
    return BRUCE_OK;
}
bruce_result_t display__begin_frame(void) {
    fakes.draw_count = 0;
    return BRUCE_OK;
}
bruce_result_t display__present(void) { return BRUCE_OK; }
bruce_result_t display__fill_screen(bruce_display_color_t color) {
    (void)color;
    return BRUCE_OK;
}
bruce_result_t display__fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bruce_display_color_t color) {
    (void)x, (void)y, (void)w, (void)h, (void)color;
    return BRUCE_OK;
}
bruce_result_t
display__fill_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color) {
    (void)x, (void)y, (void)w, (void)h, (void)r, (void)color;
    return BRUCE_OK;
}
bruce_result_t
display__draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bruce_display_color_t color) {
    (void)x, (void)y, (void)w, (void)h, (void)r, (void)color;
    return BRUCE_OK;
}
bruce_result_t display__fill_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color) {
    (void)x, (void)y, (void)r, (void)color;
    return BRUCE_OK;
}
bruce_result_t display__draw_circle(int16_t x, int16_t y, int16_t r, bruce_display_color_t color) {
    (void)x, (void)y, (void)r, (void)color;
    return BRUCE_OK;
}
bruce_result_t display__set_text_color(bruce_display_color_t color) {
    s_text_color = color;
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
bruce_result_t display__draw_string(const char *text, int16_t x, int16_t y) {
    fakes__record(false, text, x, y, s_text_color);
    return BRUCE_OK;
}
bruce_result_t display__draw_centre_string(const char *text, int16_t x, int16_t y) {
    fakes__record(false, text, x, y, s_text_color);
    return BRUCE_OK;
}
bruce_result_t display__draw_right_string(const char *text, int16_t x, int16_t y) {
    fakes__record(false, text, x, y, s_text_color);
    return BRUCE_OK;
}

static const char *s_icon_name;
static uint8_t s_icon_bits[72];

const bruce_icon_t *icon__get(const char *name) {
    static bruce_icon_t icon = {.width = 24, .height = 24, .bits = s_icon_bits};
    s_icon_name = name;
    return &icon;
}

bruce_result_t display__draw_bitmap_scaled(
    int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, int16_t out_w, int16_t out_h,
    bruce_display_color_t color
) {
    (void)bitmap, (void)w, (void)h, (void)out_w, (void)out_h;
    fakes__record(true, s_icon_name, x, y, color);
    return BRUCE_OK;
}

bruce_result_t input__read(bruce_input_event_t *out_event, uint32_t timeout_ms) {
    (void)out_event, (void)timeout_ms;
    return BRUCE_ERR_TIMEOUT;
}

bool input__wait_long_press(const bruce_input_event_t *press, uint32_t long_ms) {
    (void)press, (void)long_ms;
    return fakes.long_press;
}

bruce_result_t dialog__text_input(
    const char *title, const char *prompt, const char *initial_text, bool mask_input, char *buffer,
    size_t buffer_size
) {
    (void)title, (void)prompt, (void)initial_text, (void)mask_input;
    if (fakes.text_input_next >= FAKE_INPUTS_MAX || fakes.text_inputs[fakes.text_input_next] == NULL) {
        return BRUCE_ERR_CANCELLED;
    }
    snprintf(buffer, buffer_size, "%s", fakes.text_inputs[fakes.text_input_next++]);
    return BRUCE_OK;
}

bruce_result_t dialog__message(bruce_dialog_kind_t kind, const char *title, const char *message) {
    (void)kind, (void)title;
    fakes.message_count++;
    snprintf(fakes.message, sizeof(fakes.message), "%s", message);
    return BRUCE_OK;
}

bruce_result_t dialog__message_show(bruce_dialog_kind_t kind, const char *title, const char *message) {
    (void)kind, (void)title, (void)message;
    return BRUCE_OK;
}

bruce_result_t dialog__choice(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title, (void)message;
    fakes.choice_calls++;
    fakes.choice_count = choice_count;
    for (size_t i = 0; i < choice_count && i < 12; ++i) {
        snprintf(fakes.choice_labels[i], sizeof(fakes.choice_labels[i]), "%s", choices[i].label);
    }
    size_t answer =
        fakes.choice_next < fakes.choice_queue ? fakes.choices[fakes.choice_next++] : fakes.choice;
    *out_selected = answer < choice_count ? answer : 0;
    return BRUCE_OK;
}
