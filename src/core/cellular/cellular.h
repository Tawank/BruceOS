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
    int8_t generation; /* 2, 3 or 4 (G), from +COPS's access technology; 0 unknown. */
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

    int sms_index;           /* SIM slot of the newest +CMTI, 0 when none is pending. */
    int sms_used, sms_total; /* From AT+CPMS?. */
} cellular_model_t;

/* The header line of AT+CMGR in text mode with AT+CSDH=1. */
typedef struct {
    bool received; /* "REC READ" or "REC UNREAD", not a stored outgoing message. */
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    int64_t utc;
    int fo, dcs;
} cellular_sms_header_t;

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

/* Parses the values after "+CMGR: ". */
bool cellular__sms_header(const char *values, cellular_sms_header_t *out);

/* Decodes a message body: modem text for plain GSM 7-bit, otherwise hex
 * octets (UCS2, 8-bit, or a part of a long message with its header). */
void cellular__sms_text(const char *data, int fo, int dcs, char *out, size_t capacity);

/* Whether text can go as GSM 7-bit through the modem's text mode. */
bool cellular__sms_ascii(const char *text);

/* UTF-8 to UCS2 hex for sending; returns the number of UCS2 characters, or
 * 0 when the text is invalid or does not fit `max_chars` or `capacity`.
 * With out NULL it only checks. */
size_t cellular__sms_ucs2(const char *text, size_t max_chars, char *out, size_t capacity);

/* Mobile data for net__connect_*(): the link is up; or it is on and may still come up. */
bool cellular__data_online(void);
bool cellular__data_pending(void);

/* Cuts the modem's power before deep sleep. */
void cellular__power_off(void);

/* Powers the modem on at boot when it was left enabled. */
void cellular__start(void);
/* A call is ringing, dialling or up. */
bool cellular__in_call(void);
/* CMUX framing over the UART (mobile data): a lost byte breaks it. */
bool cellular__mux_running(void);

/* Before light sleep: the modem's RI GPIO to wake on, or -1. */
int cellular__sleep_prepare(void);
/* After light sleep, whatever woke it: asks the modem for calls and messages,
 * whose notifications may have been lost. `ring`: RI was low on waking. */
void cellular__sleep_done(bool ring);
