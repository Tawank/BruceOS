#include "gps.h"

#include <stdlib.h>
#include <string.h>

#include "hal/hal.h"
#include "core_sdk/permission.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BRUCE_GPS_ENABLED
#include "driver/uart.h"
#endif

#define GPS__FIELDS_MAX 20
#define GPS__LINE_MAX 100
#define GPS__IDLE_MS 30000
#define GPS__RECEIVING_MS 3000
#define GPS__BAUD_SWITCH_MS 3000
#define GPS__KNOTS_TO_KMH 1.852f

static int gps__hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Splits "$GPRMC,a,,b*hh" into fields in place, keeping empty ones; checks the checksum. */
static int gps__split(char *line, char **fields) {
    if (line[0] != '$') return 0;
    char *star = strchr(line, '*');
    if (star == NULL || gps__hex(star[1]) < 0 || gps__hex(star[2]) < 0) return 0;
    uint8_t sum = 0;
    for (const char *c = line + 1; c < star; ++c) sum ^= (uint8_t)*c;
    if (sum != (uint8_t)(gps__hex(star[1]) << 4 | gps__hex(star[2]))) return 0;
    *star = '\0';
    int count = 0;
    for (char *field = line + 1; count < GPS__FIELDS_MAX; ++field) {
        fields[count++] = field;
        field = strchr(field, ',');
        if (field == NULL) break;
        *field = '\0';
    }
    return count;
}

/* "ddmm.mmmm" or "dddmm.mmmm" with its hemisphere letter. */
static bool gps__coordinate(const char *value, const char *hemisphere, double *out) {
    if (value[0] == '\0' || hemisphere[0] == '\0') return false;
    double raw = strtod(value, NULL);
    double degrees = (int)(raw / 100);
    degrees += (raw - degrees * 100) / 60.0;
    *out = hemisphere[0] == 'S' || hemisphere[0] == 'W' ? -degrees : degrees;
    return true;
}

static int gps__two(const char *text) { return (text[0] - '0') * 10 + (text[1] - '0'); }

static bool gps__digits(const char *text, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    return true;
}

static bool gps__position(char **fields, int lat, bruce_gps_fix_t *fix) {
    double latitude = 0, longitude = 0;
    if (!gps__coordinate(fields[lat], fields[lat + 1], &latitude) ||
        !gps__coordinate(fields[lat + 2], fields[lat + 3], &longitude))
        return false;
    fix->latitude = latitude;
    fix->longitude = longitude;
    return true;
}

/* $xxRMC,time,status,lat,N,lon,E,knots,course,ddmmyy,... */
static bool gps__rmc(char **fields, int count, bruce_gps_fix_t *fix, bool *out_position) {
    if (count < 10) return false;
    bool active = fields[2][0] == 'A';
    if (active && gps__position(fields, 3, fix)) {
        fix->fix = true;
        *out_position = true;
        if (fields[7][0] != '\0') fix->speed_kmh = strtof(fields[7], NULL) * GPS__KNOTS_TO_KMH;
        if (fields[8][0] != '\0') fix->course_deg = strtof(fields[8], NULL);
    } else if (!active) {
        fix->fix = false;
    }
    if (gps__digits(fields[1], 6) && gps__digits(fields[9], 6)) {
        fix->utc = (bruce_clock_datetime_t){
            .year = (uint16_t)(2000 + gps__two(fields[9] + 4)),
            .month = (uint8_t)gps__two(fields[9] + 2),
            .day = (uint8_t)gps__two(fields[9]),
            .hour = (uint8_t)gps__two(fields[1]),
            .minute = (uint8_t)gps__two(fields[1] + 2),
            .second = (uint8_t)gps__two(fields[1] + 4),
        };
        fix->time_valid = true;
    }
    return true;
}

/* $xxGGA,time,lat,N,lon,E,quality,satellites,hdop,altitude,M,... */
static bool gps__gga(char **fields, int count, bruce_gps_fix_t *fix, bool *out_position) {
    if (count < 10) return false;
    fix->satellites = (uint8_t)atoi(fields[7]);
    if (fields[8][0] != '\0') fix->hdop = strtof(fields[8], NULL);
    if (atoi(fields[6]) == 0) {
        fix->fix = false;
    } else if (gps__position(fields, 2, fix)) {
        fix->fix = true;
        fix->altitude_m = strtof(fields[9], NULL);
        *out_position = true;
    }
    return true;
}

/* $xxGSV,messages,number,in view,...: the first message of each constellation's group carries its count. */
static bool gps__gsv(char **fields, int count, char talker, gps_parser_t *parser) {
    static const char talkers[GPS_PARSER_TALKERS] = {'P', 'L', 'A', 'B', 'Q', 'I'};
    if (count < 4) return false;
    const char *slot = memchr(talkers, talker, sizeof(talkers));
    if (slot == NULL || atoi(fields[2]) != 1) return true;
    parser->in_view[slot - talkers] = (uint8_t)atoi(fields[3]);
    int total = 0;
    for (int i = 0; i < GPS_PARSER_TALKERS; ++i) total += parser->in_view[i];
    parser->fix.satellites_in_view = (uint8_t)(total > 255 ? 255 : total);
    return true;
}

bool gps__parse_sentence(const char *line, gps_parser_t *parser, bool *out_position) {
    char copy[GPS__LINE_MAX];
    char *fields[GPS__FIELDS_MAX];
    size_t len = strcspn(line, "\r\n");
    *out_position = false;
    if (len >= sizeof(copy)) return false;
    memcpy(copy, line, len);
    copy[len] = '\0';
    int count = gps__split(copy, fields);
    if (count == 0 || strlen(fields[0]) != 5) return false;
    const char *type = fields[0] + 2;
    if (strcmp(type, "RMC") == 0) return gps__rmc(fields, count, &parser->fix, out_position);
    if (strcmp(type, "GGA") == 0) return gps__gga(fields, count, &parser->fix, out_position);
    if (strcmp(type, "GSV") == 0) return gps__gsv(fields, count, fields[0][1], parser);
    return false;
}

#if CONFIG_BRUCE_GPS_ENABLED

#define GPS__PORT ((uart_port_t)CONFIG_BRUCE_GPS_UART_PORT)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_on;
static uint64_t s_read_at;
static uint64_t s_data_at;
static uint64_t s_position_at;
static gps_parser_t s_parser;

static uint64_t gps__now(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* Through the board's "gps" device; nothing to do on boards that power it always. */
static void gps__power(bool on) { (void)hal_device_power(hal_device_find("gps"), on); }

void gps__power_off(void) { gps__power(false); }

static bool gps__open(void) {
    uart_config_t config = {
        .baud_rate = CONFIG_BRUCE_GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(GPS__PORT, 1024, 0, 0, NULL, 0) != ESP_OK) return false;
    if (uart_param_config(GPS__PORT, &config) != ESP_OK ||
        uart_set_pin(GPS__PORT, CONFIG_BRUCE_GPS_TX_GPIO, CONFIG_BRUCE_GPS_RX_GPIO, -1, -1) != ESP_OK) {
        uart_driver_delete(GPS__PORT);
        return false;
    }
    return true;
}

static void gps__line(const char *line) {
    bool position = false;
    portENTER_CRITICAL(&s_mux);
    gps_parser_t parser = s_parser;
    portEXIT_CRITICAL(&s_mux);
    if (!gps__parse_sentence(line, &parser, &position)) return;
    uint64_t now = gps__now();
    portENTER_CRITICAL(&s_mux);
    s_parser = parser;
    s_data_at = now;
    if (position) s_position_at = now;
    portEXIT_CRITICAL(&s_mux);
}

/* Reads sentences while someone polls gps__read(); without data it tries the other usual baud rate. */
static void gps__receive(void) {
    static const int other = CONFIG_BRUCE_GPS_BAUD == 9600 ? 38400 : 9600;
    int baud = CONFIG_BRUCE_GPS_BAUD;
    uint64_t baud_at = gps__now();
    char line[GPS__LINE_MAX];
    size_t len = 0;
    uint8_t buffer[128];
    for (;;) {
        uint64_t now = gps__now();
        portENTER_CRITICAL(&s_mux);
        bool idle = now - s_read_at > GPS__IDLE_MS;
        if (idle) s_on = false;
        uint64_t data_at = s_data_at;
        portEXIT_CRITICAL(&s_mux);
        if (idle) return;
        if (now - baud_at > GPS__BAUD_SWITCH_MS && now - data_at > GPS__BAUD_SWITCH_MS) {
            baud = baud == CONFIG_BRUCE_GPS_BAUD ? other : CONFIG_BRUCE_GPS_BAUD;
            uart_set_baudrate(GPS__PORT, baud);
            baud_at = now;
        }
        int count = uart_read_bytes(GPS__PORT, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
        for (int i = 0; i < count; ++i) {
            char c = (char)buffer[i];
            if (c == '$') len = 0;
            if (c == '\n') {
                line[len] = '\0';
                gps__line(line);
                len = 0;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = c;
            }
        }
    }
}

static void gps__task(void *context) {
    (void)context;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        gps__power(true);
        if (gps__open()) {
            gps__receive();
            uart_driver_delete(GPS__PORT);
        } else {
            portENTER_CRITICAL(&s_mux);
            s_on = false;
            portEXIT_CRITICAL(&s_mux);
        }
        gps__power(false);
        portENTER_CRITICAL(&s_mux);
        s_parser = (gps_parser_t){0};
        s_data_at = s_position_at = 0;
        portEXIT_CRITICAL(&s_mux);
    }
}

/* Created by the first reader; the others wait for it. */
static bool gps__start_task(void) {
    static bool s_creating;
    portENTER_CRITICAL(&s_mux);
    bool create = !s_creating;
    s_creating = true;
    portEXIT_CRITICAL(&s_mux);
    if (create && xTaskCreate(gps__task, "gps", 3072, NULL, 5, &s_task) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_creating = false;
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    while (s_task == NULL) vTaskDelay(1);
    return true;
}

bool gps__is_available(void) { return true; }

bruce_result_t gps__read(bruce_gps_fix_t *out) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPS);
    if (permission != BRUCE_OK) return permission;
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!gps__start_task()) return BRUCE_ERR_NO_MEMORY;
    uint64_t now = gps__now();
    portENTER_CRITICAL(&s_mux);
    s_read_at = now;
    bool wake = !s_on;
    s_on = true;
    *out = s_parser.fix;
    out->receiving = s_data_at != 0 && now - s_data_at < GPS__RECEIVING_MS;
    out->age_ms = s_position_at != 0 ? (uint32_t)(now - s_position_at) : 0;
    portEXIT_CRITICAL(&s_mux);
    if (wake) xTaskNotifyGive(s_task);
    return BRUCE_OK;
}

#else

void gps__power_off(void) {}

bool gps__is_available(void) { return false; }

bruce_result_t gps__read(bruce_gps_fix_t *out) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_GPS);
    if (permission != BRUCE_OK) return permission;
    if (out == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out = (bruce_gps_fix_t){0};
    return BRUCE_ERR_UNSUPPORTED;
}

#endif
