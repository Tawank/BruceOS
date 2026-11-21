#pragma once

/* Host fakes for the Bruce SDK calls the phone app makes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/cellular.h"
#include "core_sdk/input.h"
#include "core_sdk/result.h"

#define FAKE_DRAWS_MAX 1024
#define FAKE_FILES_MAX 4
#define FAKE_INPUTS_MAX 4

typedef struct {
    bool icon;
    int x, y;
    uint16_t color;
    char text[48];
} fake_draw_t;

typedef struct {
    char name[32];
    char data[4096];
    size_t size;
    bool used;
} fake_file_t;

typedef struct {
    int width, height;
    bool epaper;
    fake_draw_t draws[FAKE_DRAWS_MAX];
    size_t draw_count;

    fake_file_t files[FAKE_FILES_MAX];
    int writes;

    bool available;
    bruce_cellular_status_t status;
    bruce_cellular_call_record_t log[BRUCE_CELLULAR_LOG_MAX];
    size_t log_count;
    char dialed[BRUCE_CELLULAR_NUMBER_MAX];
    bruce_result_t dial_result;
    char dtmf[32];
    char ussd_code[32];
    const char *ussd_reply;
    int answers, hang_ups, enables, backgrounds;
    bool enabled;

    int64_t now_utc;
    uint64_t now_ms;
    bool long_press;
    const char *text_inputs[FAKE_INPUTS_MAX];
    size_t text_input_next;
    size_t choice;
    size_t choices[4]; /* Answers for the next dialogs, then `choice`. */
    size_t choice_queue;
    size_t choice_next;
    int choice_calls;
    char choice_labels[12][32];
    size_t choice_count;
    int message_count;
    char message[192];
} fakes_t;

extern fakes_t fakes;

void fakes__reset(void);
const fake_draw_t *fakes__find_string(const char *text);
const fake_draw_t *fakes__find_icon(const char *name);
fake_file_t *fakes__file(const char *name);
void fakes__set_file(const char *name, const char *text);
void fakes__log(bool incoming, bruce_call_result_t result, const char *number, int64_t utc, uint32_t seconds);
