#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/cellular.h" // IWYU pragma: export

#define CELLULAR__USSD_MAX 192

/* What the modem has told us so far; updated line by line. */
typedef struct {
    bool sim_ready;
    bool sim_locked;
    bool no_sim;
    int8_t creg;  /* Registration status, -1 unknown. */
    int8_t cereg; /* LTE registration status, -1 unknown. */
    int8_t signal_bars;
    char operator_name[BRUCE_CELLULAR_OPERATOR_MAX];

    bruce_call_state_t call;
    bool incoming;
    bool connected;
    bool user_hang_up;
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    uint64_t state_ms;     /* When the call state last changed. */
    uint64_t connected_ms; /* When the call was answered. */
    int64_t started_utc;
    bool clcc_seen; /* A +CLCC line came since the last AT+CLCC. */
    bool ring;      /* A new incoming call was seen. */

    bool ussd_done;
    char ussd[CELLULAR__USSD_MAX];
} cellular_model_t;

typedef struct {
    bool final; /* OK, ERROR or another final result code. */
    bool ok;
    bool ended; /* The call ended; record holds it. */
    bruce_cellular_call_record_t record;
} cellular_line_t;

void cellular__model_reset(cellular_model_t *model);

/* Applies one line from the modem (without the line ending). */
cellular_line_t cellular__parse_line(cellular_model_t *model, const char *line, uint64_t now_ms);

/* Ends the call when AT+CLCC listed no call; returns whether it did. */
bool cellular__clcc_done(cellular_model_t *model, uint64_t now_ms, bruce_cellular_call_record_t *out_record);

/* Starts tracking a call this side dialled. */
void cellular__model_dial(cellular_model_t *model, const char *number, uint64_t now_ms, int64_t utc);

bruce_cellular_state_t cellular__model_state(const cellular_model_t *model);

/* Decodes a +CUSD string; UCS2 hex when dcs says so. */
void cellular__ussd_decode(const char *text, int dcs, char *out, size_t capacity);

/* Cuts the modem's power before deep sleep. */
void cellular__power_off(void);

/* Powers the modem on at boot when it was left enabled. */
void cellular__start(void);
