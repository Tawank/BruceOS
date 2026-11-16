#include "search.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/wifi.h"
#include "geo.h"

#define SEARCH_USER_AGENT "BruceOS-Maps/0.1"
#define SEARCH_TIMEOUT_MS 15000
#define SEARCH_MAX_BYTES (64u * 1024u)

bool search__parse_coordinates(const char *text, double *out_lat, double *out_lon) {
    char *end = NULL;
    double lat = strtod(text, &end);
    if (end == text) return false;
    const char *lon_text = end;
    while (*lon_text == ',' || *lon_text == ' ') lon_text++;
    double lon = strtod(lon_text, &end);
    while (*end == ' ') end++;
    if (end == lon_text || *end != '\0') return false;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    *out_lat = lat;
    *out_lon = lon;
    return true;
}

/* ---- JSON: just enough for a flat array of objects ---------------------- */

typedef struct {
    const char *p;
    const char *end;
} json_t;

static void json__skip_space(json_t *json) {
    while (json->p < json->end && (*json->p == ' ' || *json->p == '\n' || *json->p == '\r' || *json->p == '\t')) {
        json->p++;
    }
}

static bool json__eat(json_t *json, char c) {
    json__skip_space(json);
    if (json->p >= json->end || *json->p != c) return false;
    json->p++;
    return true;
}

static int json__hex(json_t *json) {
    if (json->end - json->p < 4) return -1;
    int value = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *json->p++;
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return -1;
        value = value * 16 + digit;
    }
    return value;
}

/* Appends whole UTF-8 sequences only, so truncation never splits a character. */
static void json__put(char *out, size_t capacity, size_t *len, const char *bytes, size_t count) {
    if (out == NULL || *len + count >= capacity) return;
    memcpy(out + *len, bytes, count);
    *len += count;
    out[*len] = '\0';
}

static void json__put_code_point(char *out, size_t capacity, size_t *len, int cp) {
    char bytes[3];
    if (cp < 0x80) {
        bytes[0] = (char)cp;
        json__put(out, capacity, len, bytes, 1);
    } else if (cp < 0x800) {
        bytes[0] = (char)(0xC0 | cp >> 6);
        bytes[1] = (char)(0x80 | (cp & 0x3F));
        json__put(out, capacity, len, bytes, 2);
    } else {
        bytes[0] = (char)(0xE0 | cp >> 12);
        bytes[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        bytes[2] = (char)(0x80 | (cp & 0x3F));
        json__put(out, capacity, len, bytes, 3);
    }
}

static size_t json__utf8_length(unsigned char lead) {
    return lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
}

/* Reads a string into `out` (truncated), or skips it when `out` is NULL. */
static bool json__string(json_t *json, char *out, size_t capacity) {
    size_t len = 0;
    if (out != NULL && capacity > 0) out[0] = '\0';
    if (!json__eat(json, '"')) return false;
    while (json->p < json->end) {
        char c = *json->p++;
        if (c == '"') return true;
        if (c != '\\') {
            size_t count = json__utf8_length((unsigned char)c);
            if ((size_t)(json->end - json->p) < count - 1) return false;
            json__put(out, capacity, &len, json->p - 1, count);
            json->p += count - 1;
            continue;
        }
        if (json->p >= json->end) return false;
        char escaped = *json->p++;
        static const char from[] = "\"\\/bfnrt", to[] = "\"\\/\b\f\n\r\t";
        const char *known = strchr(from, escaped);
        if (escaped == 'u') {
            int cp = json__hex(json);
            if (cp < 0) return false;
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?'; /* Emoji and other surrogate pairs. */
            json__put_code_point(out, capacity, &len, cp);
        } else if (known != NULL && escaped != '\0') {
            json__put(out, capacity, &len, &to[known - from], 1);
        } else {
            return false;
        }
    }
    return false;
}

static bool json__skip_value(json_t *json) {
    json__skip_space(json);
    if (json->p >= json->end) return false;
    if (*json->p == '"') return json__string(json, NULL, 0);
    if (*json->p == '{' || *json->p == '[') {
        int depth = 0;
        do {
            if (json->p >= json->end) return false;
            if (*json->p == '"') {
                if (!json__string(json, NULL, 0)) return false;
                continue;
            }
            if (*json->p == '{' || *json->p == '[') depth++;
            else if (*json->p == '}' || *json->p == ']') depth--;
            json->p++;
        } while (depth > 0);
        return true;
    }
    while (json->p < json->end && strchr(",}] \n\r\t", *json->p) == NULL) json->p++;
    return true;
}

/* Nominatim sends coordinates as strings; numbers are accepted too. */
static bool json__number(json_t *json, double *out) {
    char text[32];
    json__skip_space(json);
    if (json->p < json->end && *json->p == '"') {
        if (!json__string(json, text, sizeof(text))) return false;
    } else {
        const char *start = json->p;
        if (!json__skip_value(json) || (size_t)(json->p - start) >= sizeof(text)) return false;
        memcpy(text, start, (size_t)(json->p - start));
        text[json->p - start] = '\0';
    }
    char *end = NULL;
    *out = strtod(text, &end);
    return end != text;
}

static bool search__parse_place(json_t *json, search_place_t *place) {
    bool has_lat = false, has_lon = false;
    place->name[0] = '\0';
    if (!json__eat(json, '{')) return false;
    if (json__eat(json, '}')) return false;
    do {
        char key[16];
        if (!json__string(json, key, sizeof(key)) || !json__eat(json, ':')) return false;
        bool ok = true;
        if (strcmp(key, "lat") == 0) ok = has_lat = json__number(json, &place->lat);
        else if (strcmp(key, "lon") == 0) ok = has_lon = json__number(json, &place->lon);
        else if (strcmp(key, "display_name") == 0) ok = json__string(json, place->name, sizeof(place->name));
        else ok = json__skip_value(json);
        if (!ok) return false;
    } while (json__eat(json, ','));
    return json__eat(json, '}') && has_lat && has_lon && place->name[0] != '\0';
}

size_t search__parse(const char *text, size_t len, search_place_t *places, size_t max) {
    json_t json = {.p = text, .end = text + len};
    size_t count = 0;
    if (!json__eat(&json, '[') || json__eat(&json, ']')) return 0;
    do {
        search_place_t place;
        const char *start = json.p;
        if (search__parse_place(&json, &place)) {
            if (count < max) places[count++] = place;
        } else {
            json.p = start;
            if (!json__skip_value(&json)) break;
        }
    } while (json__eat(&json, ','));
    return count;
}

/* ---- Request ------------------------------------------------------------- */

static bool search__append(char *out, size_t capacity, size_t *len, const char *text) {
    size_t n = strlen(text);
    if (*len + n >= capacity) return false;
    memcpy(out + *len, text, n + 1);
    *len += n;
    return true;
}

static bool search__append_encoded(char *out, size_t capacity, size_t *len, const char *text) {
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        char piece[4] = {(char)*c, '\0'};
        bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                     strchr("-_.~", *c) != NULL;
        if (!plain) snprintf(piece, sizeof(piece), "%%%02X", *c);
        if (!search__append(out, capacity, len, piece)) return false;
    }
    return true;
}

/* Fixed five decimals, without relying on %f support. */
static void search__format_degrees(char *out, size_t capacity, double value) {
    long scaled = (long)floor(fabs(value) * 100000.0 + 0.5);
    snprintf(out, capacity, "%s%ld.%05ld", value < 0 ? "-" : "", scaled / 100000, scaled % 100000);
}

static double search__clamp(double value, double limit) {
    return value < -limit ? -limit : value > limit ? limit : value;
}

bool search__build_url(
    const char *base, const char *query, const map_view_t *view, int width, int height, char *out, size_t capacity
) {
    double north = 0, west = 0, south = 0, east = 0;
    geo_point_t top_left = {view->center.x - width / 2, view->center.y - height / 2};
    geo_point_t bottom_right = {view->center.x + width / 2, view->center.y + height / 2};
    geo__to_lat_lon(top_left, view->zoom, &north, &west);
    geo__to_lat_lon(bottom_right, view->zoom, &south, &east);
    double box[4] = {search__clamp(west, 180), search__clamp(north, 90), search__clamp(east, 180),
                     search__clamp(south, 90)};

    size_t len = 0;
    out[0] = '\0';
    bool ok = search__append(out, capacity, &len, base) && search__append(out, capacity, &len, "?q=") &&
              search__append_encoded(out, capacity, &len, query) &&
              search__append(out, capacity, &len, "&format=jsonv2&limit=8&viewbox=");
    for (int i = 0; i < 4 && ok; ++i) {
        char degrees[16];
        search__format_degrees(degrees, sizeof(degrees), box[i]);
        ok = (i == 0 || search__append(out, capacity, &len, ",")) && search__append(out, capacity, &len, degrees);
    }
    return ok;
}

static bruce_result_t search__request(search_t *search) {
    static const char *const headers[] = {"User-Agent", SEARCH_USER_AGENT};
    bruce_http_request_t request = {
        .url = search->url,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = SEARCH_TIMEOUT_MS,
        .max_response_bytes = SEARCH_MAX_BYTES,
    };
    bruce_result_t result = http__request_start(&request, &search->job);
    search->state = result == BRUCE_OK ? SEARCH_RUNNING : SEARCH_IDLE;
    return result;
}

bruce_result_t search__start(search_t *search, const char *base, const char *query, const map_view_t *view,
                             int width, int height) {
    search__cancel(search);
    if (!search__build_url(base, query, view, width, height, search->url, sizeof(search->url))) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (wifi__is_connected()) return search__request(search);
    bruce_result_t result = wifi__connect_known_start();
    if (result == BRUCE_OK) search->state = SEARCH_CONNECTING;
    return result;
}

static bruce_result_t search__collect(search_t *search, search_place_t *places, size_t *out_count) {
    bruce_http_response_t response;
    bruce_result_t result = http__request_poll(search->job, &response);
    if (result == BRUCE_ERR_BUSY) return result;
    search->state = SEARCH_IDLE;
    search->job = BRUCE_HTTP_JOB_ID_INVALID;
    if (result != BRUCE_OK) return result;
    if (response.status_code == 200) {
        *out_count = search__parse(response.body, response.body_len, places, SEARCH_RESULTS_MAX);
    } else {
        result = BRUCE_ERR_IO;
    }
    http__response_free(&response);
    return result;
}

bruce_result_t search__poll(search_t *search, search_place_t *places, size_t *out_count) {
    *out_count = 0;
    if (search->state == SEARCH_CONNECTING) {
        bruce_result_t result = wifi__connect_known_poll();
        if (result == BRUCE_ERR_BUSY) return result;
        if (result != BRUCE_OK) {
            search->state = SEARCH_IDLE;
            return result;
        }
        result = search__request(search);
        return result == BRUCE_OK ? BRUCE_ERR_BUSY : result;
    }
    if (search->state == SEARCH_RUNNING) return search__collect(search, places, out_count);
    return BRUCE_ERR_INVALID_STATE;
}

void search__cancel(search_t *search) {
    if (search->state == SEARCH_RUNNING) http__request_cancel(search->job);
    search->state = SEARCH_IDLE;
    search->job = BRUCE_HTTP_JOB_ID_INVALID;
}
