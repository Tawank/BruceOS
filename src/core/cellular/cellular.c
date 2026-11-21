#include "cellular.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/config/config.h"
#include "core/device/device.h"
#include "core/process/process.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/clock.h"
#include "core_sdk/notification.h"
#include "core_sdk/permission.h"
#include "core_sdk/status_icon.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BRUCE_CELLULAR_ENABLED
#include "driver/uart.h"
#endif

/* ---- Parser (no I/O) ---- */

void cellular__model_reset(cellular_model_t *model) {
    memset(model, 0, sizeof(*model));
    model->creg = model->cereg = -1;
    model->signal_bars = -1;
}

static bool cellular__registered(int8_t status) { return status == 1 || status == 5; }

bruce_cellular_state_t cellular__model_state(const cellular_model_t *model) {
    if (model->no_sim) return BRUCE_CELLULAR_NO_SIM;
    if (model->sim_locked) return BRUCE_CELLULAR_SIM_LOCKED;
    if (cellular__registered(model->creg) || cellular__registered(model->cereg)) return BRUCE_CELLULAR_READY;
    if (model->creg == 3 || model->cereg == 3) return BRUCE_CELLULAR_DENIED;
    return BRUCE_CELLULAR_SEARCHING;
}

static void cellular__copy(char *out, size_t capacity, const char *from, size_t length) {
    if (length >= capacity) length = capacity - 1;
    memcpy(out, from, length);
    out[length] = '\0';
}

/* The text between the first pair of quotes after `from`. */
static bool cellular__quoted(const char *from, char *out, size_t capacity) {
    const char *start = strchr(from, '"');
    const char *end = start != NULL ? strchr(start + 1, '"') : NULL;
    if (end == NULL) return false;
    cellular__copy(out, capacity, start + 1, (size_t)(end - start - 1));
    return true;
}

static void cellular__call_state(cellular_model_t *model, bruce_call_state_t call, uint64_t now_ms) {
    if (model->call != call) model->state_ms = now_ms;
    model->call = call;
}

static void cellular__start_call(cellular_model_t *model, bool incoming, uint64_t now_ms, int64_t utc) {
    model->incoming = incoming;
    model->connected = false;
    model->user_hang_up = false;
    model->number[0] = '\0';
    model->connected_ms = 0;
    model->started_utc = utc;
    model->clcc_seen = false;
    cellular__call_state(model, incoming ? BRUCE_CALL_INCOMING : BRUCE_CALL_DIALING, now_ms);
}

void cellular__model_dial(cellular_model_t *model, const char *number, uint64_t now_ms, int64_t utc) {
    cellular__start_call(model, false, now_ms, utc);
    snprintf(model->number, sizeof(model->number), "%s", number);
}

static bruce_cellular_call_record_t
cellular__end_call(cellular_model_t *model, bruce_call_result_t explicit_result, uint64_t now_ms) {
    bruce_cellular_call_record_t record = {
        .incoming = model->incoming,
        .started_utc = model->started_utc,
    };
    snprintf(record.number, sizeof(record.number), "%s", model->number);
    if (model->connected) {
        record.result = BRUCE_CALL_RESULT_ANSWERED;
        record.seconds = (uint32_t)((now_ms - model->connected_ms) / 1000);
    } else if (model->incoming) {
        record.result = model->user_hang_up ? BRUCE_CALL_RESULT_REJECTED : BRUCE_CALL_RESULT_MISSED;
    } else if (model->user_hang_up) {
        record.result = BRUCE_CALL_RESULT_CANCELLED;
    } else {
        record.result = explicit_result;
    }
    model->call = BRUCE_CALL_NONE;
    model->state_ms = now_ms;
    model->connected = false;
    model->ring = false;
    return record;
}

static int64_t cellular__utc_now(void) {
    time_t now = time(NULL);
    return now >= 1577836800 ? (int64_t)now : 0; /* 2020-01-01 */
}

/* "+CREG: <n>,<stat>,..." answers AT+CREG?; "+CREG: <stat>,..." is a report
 * whose next field, if any, is quoted. */
static int8_t cellular__registration(const char *values) {
    int first = -1, second = -1;
    int count = sscanf(values, "%d,%d", &first, &second);
    return (int8_t)(count == 2 ? second : first);
}

static void cellular__clcc(cellular_model_t *model, const char *values, uint64_t now_ms) {
    int index = 0, direction = 0, status = 0, mode = 0, multiparty = 0;
    if (sscanf(values, "%d,%d,%d,%d,%d", &index, &direction, &status, &mode, &multiparty) < 5 || mode != 0) {
        return;
    }
    model->clcc_seen = true;
    if (status == 5) return; /* A second, waiting call. */
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    if (cellular__quoted(values, number, sizeof(number)) && number[0] != '\0') {
        snprintf(model->number, sizeof(model->number), "%s", number);
    }
    if (model->call == BRUCE_CALL_NONE)
        cellular__start_call(model, direction == 1, now_ms, cellular__utc_now());
    model->incoming = direction == 1;
    switch (status) {
        case 0:
        case 1:
            if (!model->connected) {
                model->connected = true;
                model->connected_ms = now_ms;
            }
            cellular__call_state(model, BRUCE_CALL_ACTIVE, now_ms);
            break;
        case 2: cellular__call_state(model, BRUCE_CALL_DIALING, now_ms); break;
        case 3: cellular__call_state(model, BRUCE_CALL_ALERTING, now_ms); break;
        case 4: cellular__call_state(model, BRUCE_CALL_INCOMING, now_ms); break;
        default: break;
    }
}

static int cellular__csq_bars(int rssi) {
    if (rssi < 0 || rssi == 99) return -1;
    if (rssi < 2) return 0;
    if (rssi < 10) return 1;
    if (rssi < 15) return 2;
    if (rssi < 20) return 3;
    return 4;
}

static void cellular__cusd(cellular_model_t *model, const char *values) {
    int mode = 0;
    sscanf(values, "%d", &mode);
    const char *start = strchr(values, '"');
    const char *end = start != NULL ? strrchr(start + 1, '"') : NULL;
    if (end != NULL) {
        char text[CELLULAR__USSD_MAX * 2];
        cellular__copy(text, sizeof(text), start + 1, (size_t)(end - start - 1));
        int dcs = 15;
        const char *comma = strchr(end, ',');
        if (comma != NULL) dcs = atoi(comma + 1);
        cellular__ussd_decode(text, dcs, model->ussd, sizeof(model->ussd));
    } else {
        snprintf(model->ussd, sizeof(model->ussd), "%s", mode == 4 ? "Not supported" : "Session ended");
    }
    model->ussd_done = true;
}

static bool cellular__starts(const char *line, const char *prefix, const char **out_values) {
    size_t length = strlen(prefix);
    if (strncmp(line, prefix, length) != 0) return false;
    *out_values = line + length;
    while (**out_values == ' ') ++*out_values;
    return true;
}

cellular_line_t cellular__parse_line(cellular_model_t *model, const char *line, uint64_t now_ms) {
    cellular_line_t result = {0};
    const char *values = NULL;
    if (strcmp(line, "OK") == 0) {
        result.final = result.ok = true;
    } else if (strcmp(line, "ERROR") == 0 || cellular__starts(line, "+CMS ERROR:", &values)) {
        result.final = true;
    } else if (cellular__starts(line, "+CME ERROR:", &values)) {
        result.final = true;
        if (atoi(values) == 10 || strstr(values, "not inserted") != NULL) model->no_sim = true;
    } else if (strcmp(line, "RING") == 0) {
        if (model->call == BRUCE_CALL_NONE) {
            cellular__start_call(model, true, now_ms, cellular__utc_now());
            model->ring = true;
        }
    } else if (cellular__starts(line, "+CLIP:", &values)) {
        if (model->call == BRUCE_CALL_NONE) {
            cellular__start_call(model, true, now_ms, cellular__utc_now());
            model->ring = true;
        }
        if (model->call == BRUCE_CALL_INCOMING)
            cellular__quoted(values, model->number, sizeof(model->number));
    } else if (cellular__starts(line, "+CLCC:", &values)) {
        cellular__clcc(model, values, now_ms);
    } else if (
        strcmp(line, "BUSY") == 0 || strcmp(line, "NO ANSWER") == 0 || strcmp(line, "NO CARRIER") == 0 ||
        strncmp(line, "VOICE CALL: END", 15) == 0
    ) {
        result.final = line[0] != 'V';
        if (model->call != BRUCE_CALL_NONE) {
            result.ended = true;
            result.record = cellular__end_call(
                model, line[0] == 'B' ? BRUCE_CALL_RESULT_BUSY : BRUCE_CALL_RESULT_NO_ANSWER, now_ms
            );
        }
    } else if (cellular__starts(line, "+CPIN:", &values)) {
        model->sim_ready = strcmp(values, "READY") == 0;
        model->sim_locked = strncmp(values, "SIM PIN", 7) == 0 || strncmp(values, "SIM PUK", 7) == 0;
        model->no_sim = !model->sim_ready && !model->sim_locked;
    } else if (cellular__starts(line, "+CREG:", &values)) {
        model->creg = cellular__registration(values);
    } else if (cellular__starts(line, "+CEREG:", &values)) {
        model->cereg = cellular__registration(values);
    } else if (cellular__starts(line, "+CSQ:", &values)) {
        model->signal_bars = (int8_t)cellular__csq_bars(atoi(values));
    } else if (cellular__starts(line, "+COPS:", &values)) {
        char name[BRUCE_CELLULAR_OPERATOR_MAX];
        model->operator_name[0] = '\0';
        if (cellular__quoted(values, name, sizeof(name)))
            snprintf(model->operator_name, sizeof(model->operator_name), "%s", name);
    } else if (cellular__starts(line, "+CUSD:", &values)) {
        cellular__cusd(model, values);
    } else if (cellular__starts(line, "+CMTI:", &values)) {
        const char *comma = strchr(values, ',');
        if (comma != NULL && atoi(comma + 1) > 0) model->sms_index = atoi(comma + 1);
    } else if (cellular__starts(line, "+CPMS:", &values)) {
        const char *comma = strchr(values, ',');
        if (comma != NULL) sscanf(comma + 1, "%d,%d", &model->sms_used, &model->sms_total);
    }
    return result;
}

bool cellular__clcc_done(cellular_model_t *model, uint64_t now_ms, bruce_cellular_call_record_t *out_record) {
    bool seen = model->clcc_seen;
    model->clcc_seen = false;
    if (seen || model->call == BRUCE_CALL_NONE || now_ms - model->state_ms < 3000) return false;
    *out_record = cellular__end_call(model, BRUCE_CALL_RESULT_NO_ANSWER, now_ms);
    return true;
}

static int cellular__hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static size_t cellular__utf8(uint32_t code, char *out, size_t room) {
    if (code < 0x80 && room >= 1) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800 && room >= 2) {
        out[0] = (char)(0xC0 | code >> 6);
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code >= 0x800 && code < 0x10000 && room >= 3) {
        out[0] = (char)(0xE0 | code >> 12);
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    if (code >= 0x10000 && room >= 4) {
        out[0] = (char)(0xF0 | code >> 18);
        out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code & 0x3F));
        return 4;
    }
    return 0;
}

void cellular__ussd_decode(const char *text, int dcs, char *out, size_t capacity) {
    if (capacity == 0) return;
    size_t length = strlen(text);
    bool ucs2 = (dcs == 72 || (dcs & 0xCC) == 0x48) && length % 4 == 0;
    for (size_t i = 0; ucs2 && i < length; ++i) ucs2 = cellular__hex(text[i]) >= 0;
    if (!ucs2) {
        snprintf(out, capacity, "%s", text);
        return;
    }
    size_t written = 0;
    for (size_t i = 0; i + 3 < length; i += 4) {
        uint32_t code = (uint32_t)(cellular__hex(text[i]) << 12 | cellular__hex(text[i + 1]) << 8 |
                                   cellular__hex(text[i + 2]) << 4 | cellular__hex(text[i + 3]));
        size_t used = cellular__utf8(code, out + written, capacity - 1 - written);
        if (used == 0) break;
        written += used;
    }
    out[written] = '\0';
}

/* ---- Text messages (no I/O) ---- */

/* Field `index` of a comma-separated list whose quoted fields may hold
 * commas, without its quotes. */
static bool cellular__field(const char *values, int index, char *out, size_t capacity) {
    const char *c = values;
    for (int i = 0;; ++i) {
        const char *start = c;
        bool quoted = false;
        while (*c != '\0' && (quoted || *c != ',')) {
            if (*c == '"') quoted = !quoted;
            ++c;
        }
        if (i == index) {
            const char *end = c;
            if (end - start >= 2 && *start == '"' && end[-1] == '"') {
                ++start;
                --end;
            }
            cellular__copy(out, capacity, start, (size_t)(end - start));
            return true;
        }
        if (*c == '\0') return false;
        ++c;
    }
}

/* "yy/MM/dd,hh:mm:ss+zz", local time with its offset in quarter hours. */
static int64_t cellular__sms_time(const char *scts) {
    int year, month, day, hour, minute, second, quarters = 0;
    if (sscanf(scts, "%d/%d/%d,%d:%d:%d%d", &year, &month, &day, &hour, &minute, &second, &quarters) < 6)
        return 0;
    bruce_clock_datetime_t local = {
        .year = (uint16_t)(2000 + year),
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .hour = (uint8_t)hour,
        .minute = (uint8_t)minute,
        .second = (uint8_t)second,
    };
    int64_t epoch = 0;
    if (clock__datetime_to_epoch(&local, &epoch) != BRUCE_OK) return 0;
    return epoch - (int64_t)quarters * 15 * 60;
}

bool cellular__sms_header(const char *values, cellular_sms_header_t *out) {
    memset(out, 0, sizeof(*out));
    char field[32];
    if (!cellular__field(values, 0, field, sizeof(field))) return false;
    out->received = strncmp(field, "REC ", 4) == 0;
    cellular__field(values, 1, out->number, sizeof(out->number));
    if (cellular__field(values, 3, field, sizeof(field))) out->utc = cellular__sms_time(field);
    if (cellular__field(values, 5, field, sizeof(field))) out->fo = atoi(field);
    if (cellular__field(values, 7, field, sizeof(field))) out->dcs = atoi(field);
    return true;
}

/* GSM 03.38 default alphabet; 0x1B starts the extension table. */
static const uint16_t s_gsm7[128] = {
    '@',   0xA3, '$',   0xA5,  0xE8,  0xE9,  0xF9,  0xEC,  0xF2,  0xC7,  '\n',  0xD8, 0xF8, '\r', 0xC5, 0xE5,
    0x394, '_',  0x3A6, 0x393, 0x39B, 0x3A9, 0x3A0, 0x3A8, 0x3A3, 0x398, 0x39E, 0x1B, 0xC6, 0xE6, 0xDF, 0xC9,
    ' ',   '!',  '"',   '#',   0xA4,  '%',   '&',   '\'',  '(',   ')',   '*',   '+',  ',',  '-',  '.',  '/',
    '0',   '1',  '2',   '3',   '4',   '5',   '6',   '7',   '8',   '9',   ':',   ';',  '<',  '=',  '>',  '?',
    0xA1,  'A',  'B',   'C',   'D',   'E',   'F',   'G',   'H',   'I',   'J',   'K',  'L',  'M',  'N',  'O',
    'P',   'Q',  'R',   'S',   'T',   'U',   'V',   'W',   'X',   'Y',   'Z',   0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
    0xBF,  'a',  'b',   'c',   'd',   'e',   'f',   'g',   'h',   'i',   'j',   'k',  'l',  'm',  'n',  'o',
    'p',   'q',  'r',   's',   't',   'u',   'v',   'w',   'x',   'y',   'z',   0xE4, 0xF6, 0xF1, 0xFC, 0xE0,
};

static uint16_t cellular__gsm7_extension(uint8_t septet) {
    switch (septet) {
        case 0x0A: return '\f';
        case 0x14: return '^';
        case 0x28: return '{';
        case 0x29: return '}';
        case 0x2F: return '\\';
        case 0x3C: return '[';
        case 0x3D: return '~';
        case 0x3E: return ']';
        case 0x40: return '|';
        case 0x65: return 0x20AC;
        default: return ' ';
    }
}

/* 0 GSM 7-bit, 1 8-bit, 2 UCS2 (3GPP TS 23.038). */
static int cellular__sms_alphabet(int dcs) {
    if ((dcs & 0x80) == 0) return (dcs >> 2) & 3;
    if ((dcs & 0xF0) == 0xF0) return (dcs & 0x04) != 0 ? 1 : 0;
    if ((dcs & 0xF0) == 0xE0) return 2;
    return 0;
}

void cellular__sms_text(const char *data, int fo, int dcs, char *out, size_t capacity) {
    if (capacity == 0) return;
    int alphabet = cellular__sms_alphabet(dcs);
    bool header = (fo & 0x40) != 0;
    size_t hex_length = strlen(data);
    uint8_t octets[160];
    size_t length = hex_length / 2;
    bool hex = (alphabet != 0 || header) && hex_length % 2 == 0 && length <= sizeof(octets);
    for (size_t i = 0; hex && i < length; ++i) {
        int high = cellular__hex(data[2 * i]), low = cellular__hex(data[2 * i + 1]);
        hex = high >= 0 && low >= 0;
        octets[i] = (uint8_t)(high << 4 | low);
    }
    if (!hex) {
        snprintf(out, capacity, "%s", data);
        return;
    }
    size_t skip = header && length > 0 ? (size_t)octets[0] + 1 : 0;
    if (skip > length) skip = length;
    size_t written = 0;
    if (alphabet == 2) {
        for (size_t i = skip; i + 1 < length; i += 2) {
            uint32_t code = (uint32_t)(octets[i] << 8 | octets[i + 1]);
            if (code >= 0xD800 && code < 0xDC00 && i + 3 < length) {
                uint32_t low = (uint32_t)(octets[i + 2] << 8 | octets[i + 3]);
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
            size_t used = cellular__utf8(code, out + written, capacity - 1 - written);
            if (used == 0) break;
            written += used;
        }
    } else if (alphabet == 1) {
        for (size_t i = skip; i < length; ++i) {
            size_t used = cellular__utf8(octets[i], out + written, capacity - 1 - written);
            if (used == 0) break;
            written += used;
        }
    } else {
        /* Septets packed after the header, which is padded to a septet boundary. */
        size_t septets = length * 8 / 7;
        bool escape = false;
        for (size_t s = (skip * 8 + 6) / 7; s < septets; ++s) {
            size_t bit = s * 7, byte = bit / 8, shift = bit % 8;
            unsigned value = octets[byte] >> shift;
            if (shift > 1 && byte + 1 < length) value |= (unsigned)octets[byte + 1] << (8 - shift);
            uint8_t septet = (uint8_t)(value & 0x7F);
            /* A last septet of fill bits reads as '@'. */
            if (s == septets - 1 && septet == 0 && length * 8 % 7 == 0) break;
            uint32_t code = escape ? cellular__gsm7_extension(septet) : s_gsm7[septet];
            escape = !escape && septet == 0x1B;
            if (escape) continue;
            size_t used = cellular__utf8(code, out + written, capacity - 1 - written);
            if (used == 0) break;
            written += used;
        }
    }
    out[written] = '\0';
}

bool cellular__sms_ascii(const char *text) {
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        if (*c < 0x20 || *c > 0x7E) return false;
    }
    return true;
}

size_t cellular__sms_ucs2(const char *text, size_t max_chars, char *out, size_t capacity) {
    static const char digits[] = "0123456789ABCDEF";
    size_t chars = 0, written = 0;
    const unsigned char *c = (const unsigned char *)text;
    while (*c != '\0') {
        uint32_t code;
        size_t extra;
        if (*c < 0x80) code = *c, extra = 0;
        else if ((*c & 0xE0) == 0xC0) code = *c & 0x1F, extra = 1;
        else if ((*c & 0xF0) == 0xE0) code = *c & 0x0F, extra = 2;
        else if ((*c & 0xF8) == 0xF0) code = *c & 0x07, extra = 3;
        else return 0;
        for (size_t i = 1; i <= extra; ++i) {
            if ((c[i] & 0xC0) != 0x80) return 0;
            code = code << 6 | (c[i] & 0x3F);
        }
        c += extra + 1;
        uint16_t units[2] = {(uint16_t)code, 0};
        size_t count = 1;
        if (code >= 0x10000) {
            code -= 0x10000;
            units[0] = (uint16_t)(0xD800 | code >> 10);
            units[1] = (uint16_t)(0xDC00 | (code & 0x3FF));
            count = 2;
        }
        if (chars + count > max_chars || written + 4 * count >= capacity) return 0;
        for (size_t i = 0; i < count; ++i, written += 4) {
            for (int digit = 0; out != NULL && digit < 4; ++digit)
                out[written + digit] = digits[(units[i] >> (12 - 4 * digit)) & 0xF];
        }
        chars += count;
    }
    if (out != NULL && capacity > 0) out[written] = '\0';
    return chars;
}

/* ---- Modem task ---- */

#if CONFIG_BRUCE_CELLULAR_ENABLED

#define CELLULAR__PORT ((uart_port_t)CONFIG_BRUCE_CELLULAR_UART_PORT)
/* An AT+CMGR body is up to about 300 characters of hex. */
#define CELLULAR__LINE_MAX 400
#define CELLULAR__POLL_IDLE_MS 20000
#define CELLULAR__POLL_CALL_MS 1000
#define CELLULAR__PHONE_COMMAND "GUI=1 phone incoming"

typedef enum {
    CELLULAR__REQUEST_COMMAND,
    CELLULAR__REQUEST_USSD,
    CELLULAR__REQUEST_SMS_LIST,
    CELLULAR__REQUEST_SMS_SEND,
} cellular__request_type_t;

typedef struct {
    cellular__request_type_t type;
    char command[64]; /* Or the USSD code, or the message's recipient. */
    uint32_t timeout_ms;
    char *reply;
    size_t reply_capacity;
    const char *text;                   /* SMS_SEND */
    bruce_cellular_message_t *messages; /* SMS_LIST */
    size_t message_capacity, message_count;
    bruce_result_t result;
    SemaphoreHandle_t done;
} cellular__request_t;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static QueueHandle_t s_requests;
static bool s_enabled;
static bool s_powered;
static bool s_alive;
static bool s_failed;
static bool s_muted;
static uint8_t s_volume = 60;
static cellular_model_t s_model;
static bruce_cellular_call_record_t s_log[BRUCE_CELLULAR_LOG_MAX];
static uint32_t s_log_sequence;
static uint32_t s_message_sequence;
static char s_line[CELLULAR__LINE_MAX];
static size_t s_line_length;

static uint64_t cellular__now(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void cellular__lock(void) {
    if (s_lock == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void cellular__unlock(void) { xSemaphoreGive(s_lock); }

static void cellular__audio(bool modem) {
#if CONFIG_BRUCE_CELLULAR_AUDIO_XL9555_PIN >= 0
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_AUDIO_XL9555_PIN, modem);
#else
    (void)modem;
#endif
}

void cellular__power_off(void) {
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_PWRKEY_XL9555_PIN, false);
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_POWER_XL9555_PIN, false);
    cellular__audio(false);
}

static bool cellular__read_line(char *out, size_t capacity, uint32_t timeout_ms) {
    uint64_t deadline = cellular__now() + timeout_ms;
    for (;;) {
        uint8_t c;
        uint64_t now = cellular__now();
        uint32_t wait = now < deadline ? (uint32_t)(deadline - now) : 0;
        if (uart_read_bytes(CELLULAR__PORT, &c, 1, pdMS_TO_TICKS(wait)) != 1) return false;
        if (c == '\r') continue;
        if (c == '\n') {
            if (s_line_length == 0) continue;
            s_line[s_line_length] = '\0';
            snprintf(out, capacity, "%s", s_line);
            s_line_length = 0;
            return true;
        }
        if (s_line_length + 1 < sizeof(s_line)) s_line[s_line_length++] = (char)c;
    }
}

static void cellular__log_call(const bruce_cellular_call_record_t *record) {
    cellular__lock();
    memmove(&s_log[0], &s_log[1], sizeof(s_log) - sizeof(s_log[0]));
    s_log[BRUCE_CELLULAR_LOG_MAX - 1] = *record;
    s_log[BRUCE_CELLULAR_LOG_MAX - 1].sequence = ++s_log_sequence;
    cellular__unlock();
    cellular__audio(false);
    if (record->result == BRUCE_CALL_RESULT_MISSED) {
        char text[BRUCE_NOTIFICATION_TEXT_MAX];
        snprintf(
            text, sizeof(text), "Missed call: %s", record->number[0] != '\0' ? record->number : "unknown"
        );
        notification__push(text, BRUCE_NOTIFICATION_DURATION_MAX_MS);
    }
}

/* Applies one line; returns it for the caller to check for a final result. */
static cellular_line_t cellular__handle_line(const char *line) {
    cellular__lock();
    cellular_line_t result = cellular__parse_line(&s_model, line, cellular__now());
    cellular__unlock();
    if (result.ended) cellular__log_call(&result.record);
    return result;
}

static void cellular__write(const char *command) {
    uart_write_bytes(CELLULAR__PORT, command, strlen(command));
    uart_write_bytes(CELLULAR__PORT, "\r", 1);
}

/* Reads lines until a final result. */
static bool cellular__finish(uint32_t timeout_ms) {
    uint64_t deadline = cellular__now() + timeout_ms;
    char line[CELLULAR__LINE_MAX];
    for (uint64_t now = cellular__now(); now < deadline; now = cellular__now()) {
        if (!cellular__read_line(line, sizeof(line), (uint32_t)(deadline - now))) break;
        cellular_line_t result = cellular__handle_line(line);
        if (result.final) return result.ok;
    }
    return false;
}

/* Sends one command and reads lines until its final result. */
static bool cellular__command(const char *command, uint32_t timeout_ms) {
    cellular__write(command);
    return cellular__finish(timeout_ms);
}

static bool cellular__open(void) {
    uart_config_t config = {
        .baud_rate = CONFIG_BRUCE_CELLULAR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(CELLULAR__PORT, 2048, 0, 0, NULL, 0) != ESP_OK) return false;
    if (uart_param_config(CELLULAR__PORT, &config) != ESP_OK ||
        uart_set_pin(CELLULAR__PORT, CONFIG_BRUCE_CELLULAR_TX_GPIO, CONFIG_BRUCE_CELLULAR_RX_GPIO, -1, -1) !=
            ESP_OK) {
        uart_driver_delete(CELLULAR__PORT);
        return false;
    }
    return true;
}

/* LilyGO's sequence: supply on with PWRKEY released, and pulse PWRKEY only
 * when the modem does not already answer. */
static bool cellular__power_on(void) {
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_PWRKEY_XL9555_PIN, false);
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_POWER_XL9555_PIN, true);
    s_powered = true;
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (cellular__command("AT", 1500)) return true;
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_PWRKEY_XL9555_PIN, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_PWRKEY_XL9555_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(1100));
    (void)device__xl9555_set(CONFIG_BRUCE_CELLULAR_PWRKEY_XL9555_PIN, false);
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (cellular__command("AT", 1000)) return true;
    }
    return false;
}

static void cellular__query_network(void) {
    cellular__command("AT+CPIN?", 2000);
    cellular__command("AT+CREG?", 2000);
    cellular__command("AT+CEREG?", 2000);
    cellular__command("AT+COPS?", 5000);
    cellular__command("AT+CSQ", 2000);
}

static void cellular__set_volume_level(void) {
    char command[16];
    snprintf(command, sizeof(command), "AT+CLVL=%u", (unsigned)((s_volume * 5 + 50) / 100));
    cellular__command(command, 2000);
}

static void cellular__setup(void) {
    cellular__command("ATE0", 1000);
    cellular__command("AT+CMEE=1", 1000);
    cellular__command("AT+CLIP=1", 1000);
    cellular__command("AT+CREG=1", 1000);
    cellular__command("AT+CEREG=1", 1000);
    cellular__command("AT+COPS=3,0", 2000);
    cellular__command("AT+CSCS=\"IRA\"", 1000);
    cellular__command("AT+CMGF=1", 1000);
    cellular__command("AT+CSDH=1", 1000);
    cellular__command("AT+CNMI=2,1,0,0,0", 1000);
    cellular__set_volume_level();
    cellular__query_network();
}

static void cellular__incoming(void) {
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    cellular__lock();
    bool ring = s_model.ring;
    s_model.ring = false;
    snprintf(number, sizeof(number), "%s", s_model.number);
    cellular__unlock();
    if (!ring) return;
    cellular__audio(true);
    char text[BRUCE_NOTIFICATION_TEXT_MAX];
    snprintf(text, sizeof(text), "Incoming call: %s", number[0] != '\0' ? number : "unknown");
    notification__push(text, BRUCE_NOTIFICATION_DURATION_MAX_MS);
    /* The phone app is single-instance: a running one comes to the front. */
    (void)app_runner__run_command(CELLULAR__PHONE_COMMAND, BRUCE_LAUNCH_FOREGROUND);
}

static bool cellular__in_call(void) {
    cellular__lock();
    bool in_call = s_model.call != BRUCE_CALL_NONE;
    cellular__unlock();
    return in_call;
}

static void cellular__poll_call(void) {
    cellular__lock();
    s_model.clcc_seen = false;
    cellular__unlock();
    if (!cellular__command("AT+CLCC", 2000)) return;
    bruce_cellular_call_record_t record;
    cellular__lock();
    bool ended = cellular__clcc_done(&s_model, cellular__now(), &record);
    cellular__unlock();
    if (ended) cellular__log_call(&record);
}

static void cellular__ussd_request(cellular__request_t *request) {
    cellular__lock();
    s_model.ussd_done = false;
    cellular__unlock();
    char command[96];
    snprintf(command, sizeof(command), "AT+CUSD=1,\"%s\",15", request->command);
    if (!cellular__command(command, 5000)) {
        request->result = BRUCE_ERR_IO;
        return;
    }
    uint64_t deadline = cellular__now() + request->timeout_ms;
    char line[CELLULAR__LINE_MAX];
    request->result = BRUCE_ERR_TIMEOUT;
    for (uint64_t now = cellular__now(); now < deadline; now = cellular__now()) {
        if (cellular__read_line(line, sizeof(line), (uint32_t)(deadline - now))) cellular__handle_line(line);
        cellular__lock();
        bool done = s_model.ussd_done;
        if (done) snprintf(request->reply, request->reply_capacity, "%s", s_model.ussd);
        cellular__unlock();
        if (done) {
            request->result = BRUCE_OK;
            break;
        }
    }
    if (request->result != BRUCE_OK) cellular__command("AT+CUSD=2", 2000);
}

static bool cellular__is_final(const char *line) {
    return strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0 || strncmp(line, "+CMS ERROR:", 11) == 0 ||
           strncmp(line, "+CME ERROR:", 11) == 0;
}

/* Reads SIM slot `index`; returns whether it holds a received message.
 * *out_stored says whether the slot held any message. */
static bool cellular__read_sms(int index, bruce_cellular_message_t *out, bool *out_stored) {
    static char s_body[CELLULAR__LINE_MAX];
    char line[CELLULAR__LINE_MAX];
    snprintf(line, sizeof(line), "AT+CMGR=%d", index);
    cellular__write(line);
    cellular_sms_header_t header = {0};
    bool stored = false;
    size_t body_length = 0;
    s_body[0] = '\0';
    uint64_t deadline = cellular__now() + 5000;
    for (uint64_t now = cellular__now(); now < deadline; now = cellular__now()) {
        if (!cellular__read_line(line, sizeof(line), (uint32_t)(deadline - now))) break;
        if (!stored && strncmp(line, "+CMGR:", 6) == 0) {
            stored = cellular__sms_header(line + 6 + strspn(line + 6, " "), &header);
        } else if (cellular__is_final(line)) {
            cellular__handle_line(line);
            break;
        } else if (stored) {
            /* A message with line breaks comes as several lines. */
            int written = snprintf(
                s_body + body_length, sizeof(s_body) - body_length, "%s%s", body_length > 0 ? "\n" : "", line
            );
            if (written > 0) body_length = strnlen(s_body, sizeof(s_body) - 1);
        } else {
            cellular__handle_line(line);
        }
    }
    *out_stored = stored;
    if (!stored || !header.received) return false;
    memset(out, 0, sizeof(*out));
    out->id = (uint16_t)index;
    out->received_utc = header.utc;
    snprintf(out->number, sizeof(out->number), "%s", header.number);
    cellular__sms_text(s_body, header.fo, header.dcs, out->text, sizeof(out->text));
    return true;
}

static void cellular__sms_list_request(cellular__request_t *request) {
    cellular__lock();
    s_model.sms_used = s_model.sms_total = 0;
    cellular__unlock();
    if (!cellular__command("AT+CPMS?", 3000)) {
        request->result = BRUCE_ERR_IO;
        return;
    }
    cellular__lock();
    int used = s_model.sms_used, total = s_model.sms_total;
    cellular__unlock();
    int found = 0;
    for (int index = 1; index <= total && found < used && request->message_count < request->message_capacity;
         ++index) {
        bool stored = false;
        if (cellular__read_sms(index, &request->messages[request->message_count], &stored))
            request->message_count++;
        if (stored) found++;
    }
    request->result = BRUCE_OK;
}

/* Waits for the "> " that asks for the message text. */
static bool cellular__wait_prompt(uint32_t timeout_ms) {
    uint64_t deadline = cellular__now() + timeout_ms;
    for (uint64_t now = cellular__now(); now < deadline; now = cellular__now()) {
        uint8_t c;
        if (uart_read_bytes(CELLULAR__PORT, &c, 1, pdMS_TO_TICKS(deadline - now)) != 1) return false;
        if (c == '>' && s_line_length == 0) return true;
        if (c == '\r') continue;
        if (c != '\n') {
            if (s_line_length + 1 < sizeof(s_line)) s_line[s_line_length++] = (char)c;
            continue;
        }
        if (s_line_length == 0) continue;
        char line[CELLULAR__LINE_MAX];
        s_line[s_line_length] = '\0';
        snprintf(line, sizeof(line), "%s", s_line);
        s_line_length = 0;
        if (cellular__handle_line(line).final) return false;
    }
    return false;
}

static void cellular__sms_send_request(cellular__request_t *request) {
    static char s_hex[BRUCE_CELLULAR_SMS_UCS2_MAX * 4 + 1];
    bool ascii = cellular__sms_ascii(request->text);
    if (!ascii) cellular__sms_ucs2(request->text, BRUCE_CELLULAR_SMS_UCS2_MAX, s_hex, sizeof(s_hex));
    request->result = BRUCE_ERR_IO;
    if (!cellular__command(ascii ? "AT+CSMP=17,167,0,0" : "AT+CSMP=17,167,0,8", 2000)) return;
    char command[sizeof(request->command) + 16];
    snprintf(command, sizeof(command), "AT+CMGS=\"%s\"", request->command);
    cellular__write(command);
    if (!cellular__wait_prompt(5000)) {
        uart_write_bytes(CELLULAR__PORT, "\x1B", 1); /* ESC cancels. */
        cellular__finish(2000);
        return;
    }
    const char *body = ascii ? request->text : s_hex;
    uart_write_bytes(CELLULAR__PORT, body, strlen(body));
    uart_write_bytes(CELLULAR__PORT, "\x1A", 1); /* Ctrl-Z sends. */
    if (cellular__finish(60000)) request->result = BRUCE_OK;
}

/* A new message: tell the user, and apps watching message_sequence. */
static void cellular__sms_arrived(void) {
    cellular__lock();
    int index = s_model.sms_index;
    s_model.sms_index = 0;
    cellular__unlock();
    if (index <= 0) return;
    static bruce_cellular_message_t s_message;
    bool stored = false;
    char text[BRUCE_NOTIFICATION_TEXT_MAX];
    if (cellular__read_sms(index, &s_message, &stored)) {
        /* The notification shows the start of the message. */
        snprintf(text, sizeof(text), "%.31s: %.*s", s_message.number, (int)sizeof(text) - 34, s_message.text);
    } else {
        snprintf(text, sizeof(text), "New message");
    }
    cellular__lock();
    s_message_sequence++;
    cellular__unlock();
    notification__push(text, BRUCE_NOTIFICATION_DURATION_MAX_MS);
}

static void cellular__run_request(cellular__request_t *request) {
    if (!s_alive) {
        request->result = BRUCE_ERR_INVALID_STATE;
    } else if (request->type == CELLULAR__REQUEST_USSD) {
        cellular__ussd_request(request);
    } else if (request->type == CELLULAR__REQUEST_SMS_LIST) {
        cellular__sms_list_request(request);
    } else if (request->type == CELLULAR__REQUEST_SMS_SEND) {
        cellular__sms_send_request(request);
    } else {
        request->result = cellular__command(request->command, request->timeout_ms) ? BRUCE_OK : BRUCE_ERR_IO;
    }
    xSemaphoreGive(request->done);
}

#define CELLULAR__STATUS_ICON_KEY "core.cellular"

/* Signal bars in the status bar while the modem is on. Runs on the modem task. */
static void cellular__update_status_icon(void) {
    static const char *const icons[] = {
        "signal-cellular-outline",
        "signal-cellular-1",
        "signal-cellular-2",
        "signal-cellular-3",
        "signal-cellular-3",
    };
    static const char *s_shown;
    const char *icon = NULL;
    if (s_enabled && !s_failed) {
        bool ready = s_alive && cellular__model_state(&s_model) == BRUCE_CELLULAR_READY;
        int bars = ready ? s_model.signal_bars : 0;
        icon = icons[bars > 0 && bars <= 4 ? bars : 0];
    }
    if (icon == s_shown) return;
    s_shown = icon;
    if (icon != NULL) {
        (void)status_icon__push_named(CELLULAR__STATUS_ICON_KEY, icon);
    } else {
        (void)status_icon__remove(CELLULAR__STATUS_ICON_KEY);
    }
}

/* Runs while the modem is enabled. */
static void cellular__session(void) {
    s_failed = false;
    s_alive = cellular__power_on();
    if (!s_alive) {
        s_failed = true;
        return;
    }
    cellular__setup();
    uint64_t polled_at = cellular__now();
    char line[CELLULAR__LINE_MAX];
    while (s_enabled) {
        cellular__request_t *request = NULL;
        if (xQueueReceive(s_requests, &request, 0) == pdTRUE) cellular__run_request(request);
        if (cellular__read_line(line, sizeof(line), 100)) cellular__handle_line(line);
        cellular__incoming();
        cellular__sms_arrived();
        cellular__update_status_icon();
        uint64_t now = cellular__now();
        bool in_call = cellular__in_call();
        if (now - polled_at >= (in_call ? CELLULAR__POLL_CALL_MS : CELLULAR__POLL_IDLE_MS)) {
            if (in_call) {
                cellular__poll_call();
            } else {
                cellular__query_network();
            }
            polled_at = cellular__now();
        }
    }
    if (cellular__in_call()) cellular__command("AT+CHUP", 2000);
}

static void cellular__fail_requests(void) {
    cellular__request_t *request = NULL;
    while (xQueueReceive(s_requests, &request, 0) == pdTRUE) {
        request->result = BRUCE_ERR_INVALID_STATE;
        xSemaphoreGive(request->done);
    }
}

static void cellular__task(void *context) {
    (void)context;
    for (;;) {
        cellular__update_status_icon();
        if (!s_enabled) {
            cellular__fail_requests();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        if (!cellular__open()) {
            s_failed = true;
            cellular__update_status_icon();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        s_line_length = 0;
        cellular__session();
        uart_driver_delete(CELLULAR__PORT);
        cellular__power_off();
        s_powered = s_alive = false;
        cellular__lock();
        cellular__model_reset(&s_model);
        cellular__unlock();
        cellular__fail_requests();
        cellular__update_status_icon();
        /* A modem that did not answer stays off until it is enabled again. */
        while (s_failed && s_enabled) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static bool cellular__ensure_task(void) {
    static bool s_creating;
    portENTER_CRITICAL(&s_init_mux);
    bool create = !s_creating;
    s_creating = true;
    portEXIT_CRITICAL(&s_init_mux);
    if (create) {
        cellular__lock();
        cellular__model_reset(&s_model);
        cellular__unlock();
        s_requests = xQueueCreate(4, sizeof(cellular__request_t *));
        if (s_requests == NULL || xTaskCreate(cellular__task, "cellular", 6144, NULL, 5, &s_task) != pdPASS) {
            portENTER_CRITICAL(&s_init_mux);
            s_creating = false;
            portEXIT_CRITICAL(&s_init_mux);
            return false;
        }
    }
    while (s_task == NULL) vTaskDelay(1);
    return true;
}

void cellular__start(void) {
    if (!config__get_cellular_enabled()) return;
    s_enabled = true;
    if (cellular__ensure_task()) xTaskNotifyGive(s_task);
}

bool cellular__is_available(void) { return true; }

bruce_result_t cellular__set_enabled(bool enabled) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (!cellular__ensure_task()) return BRUCE_ERR_NO_MEMORY;
    if (enabled != config__get_cellular_enabled()) (void)config__set_cellular_enabled(enabled);
    if (enabled) s_failed = false;
    s_enabled = enabled;
    xTaskNotifyGive(s_task);
    return BRUCE_OK;
}

bruce_result_t cellular__get_status(bruce_cellular_status_t *out) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    cellular__lock();
    if (!s_enabled) {
        out->state = BRUCE_CELLULAR_OFF;
    } else if (s_failed) {
        out->state = BRUCE_CELLULAR_FAILED;
    } else if (!s_alive) {
        out->state = BRUCE_CELLULAR_STARTING;
    } else {
        out->state = cellular__model_state(&s_model);
    }
    out->signal_bars = s_alive ? s_model.signal_bars : -1;
    snprintf(out->operator_name, sizeof(out->operator_name), "%s", s_model.operator_name);
    out->call = s_model.call;
    out->call_incoming = s_model.incoming;
    snprintf(out->call_number, sizeof(out->call_number), "%s", s_model.number);
    if (s_model.call == BRUCE_CALL_ACTIVE)
        out->call_seconds = (uint32_t)((cellular__now() - s_model.connected_ms) / 1000);
    out->muted = s_muted;
    out->volume = s_volume;
    out->last_log_sequence = s_log_sequence;
    out->message_sequence = s_message_sequence;
    cellular__unlock();
    return BRUCE_OK;
}

/* Hands a request to the modem task and waits for it. */
static bruce_result_t cellular__submit(cellular__request_t *request) {
    if (!s_enabled || s_requests == NULL) return BRUCE_ERR_INVALID_STATE;
    StaticSemaphore_t done_storage;
    request->result = BRUCE_ERR_INVALID_STATE;
    request->done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (!process_registry__operation_begin()) return BRUCE_ERR_INVALID_STATE;
    bruce_result_t result = BRUCE_ERR_BUSY;
    if (xQueueSend(s_requests, &request, pdMS_TO_TICKS(1000)) == pdTRUE) {
        xTaskNotifyGive(s_task);
        xSemaphoreTake(request->done, portMAX_DELAY);
        result = request->result;
    }
    process_registry__operation_end();
    return result;
}

static bruce_result_t cellular__request(
    cellular__request_type_t type, const char *command, uint32_t timeout_ms, char *reply, size_t capacity
) {
    cellular__request_t request = {
        .type = type,
        .timeout_ms = timeout_ms,
        .reply = reply,
        .reply_capacity = capacity,
    };
    snprintf(request.command, sizeof(request.command), "%s", command);
    return cellular__submit(&request);
}

static bool cellular__valid_number(const char *number) {
    if (number == NULL || number[0] == '\0' || strlen(number) >= BRUCE_CELLULAR_NUMBER_MAX) return false;
    for (const char *c = number; *c != '\0'; ++c) {
        if (!isdigit((unsigned char)*c) && *c != '*' && *c != '#' && !(*c == '+' && c == number))
            return false;
    }
    return true;
}

bruce_result_t cellular__dial(const char *number) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (!cellular__valid_number(number)) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!s_alive) return BRUCE_ERR_INVALID_STATE;
    if (cellular__in_call()) return BRUCE_ERR_BUSY;
    cellular__lock();
    cellular__model_dial(&s_model, number, cellular__now(), cellular__utc_now());
    cellular__unlock();
    cellular__audio(true);
    char command[48];
    snprintf(command, sizeof(command), "ATD%s;", number);
    bruce_result_t result = cellular__request(CELLULAR__REQUEST_COMMAND, command, 10000, NULL, 0);
    if (result != BRUCE_OK) {
        cellular__lock();
        bool ended = s_model.call != BRUCE_CALL_NONE;
        bruce_cellular_call_record_t record =
            ended ? cellular__end_call(&s_model, BRUCE_CALL_RESULT_FAILED, cellular__now())
                  : (bruce_cellular_call_record_t){0};
        cellular__unlock();
        if (ended) cellular__log_call(&record);
    }
    return result;
}

bruce_result_t cellular__answer(void) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    cellular__lock();
    bool incoming = s_model.call == BRUCE_CALL_INCOMING;
    cellular__unlock();
    if (!incoming) return BRUCE_ERR_INVALID_STATE;
    cellular__audio(true);
    return cellular__request(CELLULAR__REQUEST_COMMAND, "ATA", 10000, NULL, 0);
}

bruce_result_t cellular__hang_up(void) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    cellular__lock();
    bool in_call = s_model.call != BRUCE_CALL_NONE;
    s_model.user_hang_up = in_call;
    cellular__unlock();
    if (!in_call) return BRUCE_ERR_INVALID_STATE;
    return cellular__request(CELLULAR__REQUEST_COMMAND, "AT+CHUP", 5000, NULL, 0);
}

bruce_result_t cellular__send_dtmf(char digit) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (!isdigit((unsigned char)digit) && digit != '*' && digit != '#') return BRUCE_ERR_INVALID_ARGUMENT;
    char command[16];
    snprintf(command, sizeof(command), "AT+VTS=%c", digit);
    return cellular__request(CELLULAR__REQUEST_COMMAND, command, 3000, NULL, 0);
}

bruce_result_t cellular__set_muted(bool muted) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    bruce_result_t result =
        cellular__request(CELLULAR__REQUEST_COMMAND, muted ? "AT+CMUT=1" : "AT+CMUT=0", 2000, NULL, 0);
    if (result == BRUCE_OK) s_muted = muted;
    return result;
}

bruce_result_t cellular__set_volume(uint8_t volume) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (volume > 100) volume = 100;
    s_volume = volume;
    char command[16];
    snprintf(command, sizeof(command), "AT+CLVL=%u", (unsigned)((volume * 5 + 50) / 100));
    return cellular__request(CELLULAR__REQUEST_COMMAND, command, 2000, NULL, 0);
}

bruce_result_t cellular__ussd(const char *code, char *out_reply, size_t capacity, uint32_t timeout_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (!cellular__valid_number(code) || out_reply == NULL || capacity == 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    out_reply[0] = '\0';
    return cellular__request(CELLULAR__REQUEST_USSD, code, timeout_ms, out_reply, capacity);
}

bruce_result_t cellular__get_messages(bruce_cellular_message_t *out, size_t capacity, size_t *out_count) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (out_count == NULL || (out == NULL && capacity > 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    cellular__request_t request = {
        .type = CELLULAR__REQUEST_SMS_LIST,
        .messages = out,
        .message_capacity = capacity,
    };
    bruce_result_t result = cellular__submit(&request);
    *out_count = request.message_count;
    return result;
}

bruce_result_t cellular__delete_message(uint16_t id) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (id == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    char command[24];
    snprintf(command, sizeof(command), "AT+CMGD=%u", (unsigned)id);
    return cellular__request(CELLULAR__REQUEST_COMMAND, command, 5000, NULL, 0);
}

bruce_result_t cellular__send_message(const char *number, const char *text) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    if (permission != BRUCE_OK) return permission;
    if (!cellular__valid_number(number) || text == NULL || text[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    if (cellular__sms_ascii(text)
            ? strlen(text) > BRUCE_CELLULAR_SMS_ASCII_MAX
            : cellular__sms_ucs2(text, BRUCE_CELLULAR_SMS_UCS2_MAX, NULL, SIZE_MAX) == 0) {
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    cellular__request_t request = {.type = CELLULAR__REQUEST_SMS_SEND, .text = text};
    snprintf(request.command, sizeof(request.command), "%s", number);
    return cellular__submit(&request);
}

size_t cellular__get_call_log(uint32_t after_sequence, bruce_cellular_call_record_t *out, size_t capacity) {
    if (permission__check(BRUCE_PERMISSION_PHONE) != BRUCE_OK || out == NULL) return 0;
    size_t count = 0;
    cellular__lock();
    for (size_t i = 0; i < BRUCE_CELLULAR_LOG_MAX && count < capacity; ++i) {
        if (s_log[i].sequence > after_sequence) out[count++] = s_log[i];
    }
    cellular__unlock();
    return count;
}

#else

void cellular__power_off(void) {}

void cellular__start(void) {}

bool cellular__is_available(void) { return false; }

static bruce_result_t cellular__unsupported(void) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_PHONE);
    return permission != BRUCE_OK ? permission : BRUCE_ERR_UNSUPPORTED;
}

bruce_result_t cellular__set_enabled(bool enabled) {
    (void)enabled;
    return cellular__unsupported();
}

bruce_result_t cellular__get_status(bruce_cellular_status_t *out) {
    if (out != NULL) memset(out, 0, sizeof(*out));
    return cellular__unsupported();
}

bruce_result_t cellular__dial(const char *number) {
    (void)number;
    return cellular__unsupported();
}

bruce_result_t cellular__answer(void) { return cellular__unsupported(); }

bruce_result_t cellular__hang_up(void) { return cellular__unsupported(); }

bruce_result_t cellular__send_dtmf(char digit) {
    (void)digit;
    return cellular__unsupported();
}

bruce_result_t cellular__set_muted(bool muted) {
    (void)muted;
    return cellular__unsupported();
}

bruce_result_t cellular__set_volume(uint8_t volume) {
    (void)volume;
    return cellular__unsupported();
}

bruce_result_t cellular__ussd(const char *code, char *out_reply, size_t capacity, uint32_t timeout_ms) {
    (void)code;
    (void)timeout_ms;
    if (out_reply != NULL && capacity > 0) out_reply[0] = '\0';
    return cellular__unsupported();
}

bruce_result_t cellular__get_messages(bruce_cellular_message_t *out, size_t capacity, size_t *out_count) {
    (void)out;
    (void)capacity;
    if (out_count != NULL) *out_count = 0;
    return cellular__unsupported();
}

bruce_result_t cellular__delete_message(uint16_t id) {
    (void)id;
    return cellular__unsupported();
}

bruce_result_t cellular__send_message(const char *number, const char *text) {
    (void)number;
    (void)text;
    return cellular__unsupported();
}

size_t cellular__get_call_log(uint32_t after_sequence, bruce_cellular_call_record_t *out, size_t capacity) {
    (void)after_sequence;
    (void)out;
    (void)capacity;
    return 0;
}

#endif
