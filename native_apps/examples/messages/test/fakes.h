#pragma once

/* Host fakes for the Bruce SDK calls the messages app makes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/cellular.h"
#include "core_sdk/result.h"

#define FAKE_DRAWS_MAX 1024
#define FAKE_FILES_MAX 4
#define FAKE_SIM_MAX 8
#define FAKE_INPUTS_MAX 4

typedef struct {
    bool icon;
    int x, y;
    uint16_t color;
    char text[64];
} fake_draw_t;

typedef struct {
    char path[64];
    char data[65536];
    size_t size;
    bool used;
} fake_file_t;

typedef struct {
    int width, height;
    bool epaper;
    fake_draw_t draws[FAKE_DRAWS_MAX];
    size_t draw_count;

    fake_file_t files[FAKE_FILES_MAX];
    int saves;

    bruce_cellular_status_t status;
    bruce_cellular_message_t sim[FAKE_SIM_MAX]; /* Messages on the SIM. */
    size_t sim_count;
    int deletes;
    char sent_number[BRUCE_CELLULAR_NUMBER_MAX];
    char sent_text[BRUCE_CELLULAR_TEXT_MAX];
    bruce_result_t send_result;

    int64_t now_utc;
    uint64_t now_ms;
    bool long_press;
    const char *text_inputs[FAKE_INPUTS_MAX];
    size_t text_input_next;
    size_t choice;
    size_t choices[4]; /* Answers for the next dialogs, then `choice`. */
    size_t choice_queue, choice_next;
    int choice_calls;
    char choice_labels[8][32];
    size_t choice_count;
    int message_count;
    char message[192];
} fakes_t;

extern fakes_t fakes;

void fakes__reset(void);
const fake_draw_t *fakes__find_string(const char *text);
const fake_draw_t *fakes__find_icon(const char *name);
fake_file_t *fakes__file(const char *path);
void fakes__set_file(const char *path, const char *text);
void fakes__receive(const char *number, const char *text, int64_t utc);
