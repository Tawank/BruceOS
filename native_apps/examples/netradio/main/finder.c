#include "finder.h"

#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/net.h"
#include "json.h"

#define FINDER_USER_AGENT "BruceOS-InternetRadio/0.1"
#define FINDER_TIMEOUT_MS 15000

bool finder__build_url(const char *base, const char *query, char *out, size_t capacity) {
    int used = snprintf(out, capacity, "%s/json/stations/search?limit=%d&hidebroken=true&order=clickcount&reverse=true&name=",
                        base, FINDER_RESULTS_MAX + 5);
    if (used < 0 || (size_t)used >= capacity) return false;
    for (const unsigned char *c = (const unsigned char *)query; *c != '\0'; ++c) {
        char piece[4] = {(char)*c, '\0'};
        if (!isalnum(*c) && strchr("-_.~", *c) == NULL) snprintf(piece, sizeof(piece), "%%%02X", *c);
        size_t length = strlen(piece);
        if ((size_t)used + length >= capacity) return false;
        memcpy(out + used, piece, length + 1);
        used += (int)length;
    }
    return true;
}

/* Radio Browser's codec names to the extensions audio__url_play() takes. */
static void finder__codec(const char *name, char *out) {
    out[0] = '\0';
    if (strcasecmp(name, "MP3") == 0) strcpy(out, "mp3");
    else if (strncasecmp(name, "AAC", 3) == 0) strcpy(out, "aac");
    else if (strcasecmp(name, "OGG") == 0) strcpy(out, "ogg");
    else if (strcasecmp(name, "FLAC") == 0) strcpy(out, "flac");
}

static bool finder__parse_station(json_t *json, station_t *station) {
    char codec[16] = "";
    double hls = 0;
    memset(station, 0, sizeof(*station));
    if (!json__eat(json, '{')) return false;
    if (json__eat(json, '}')) return false;
    do {
        char key[24];
        if (!json__string(json, key, sizeof(key)) || !json__eat(json, ':')) return false;
        bool ok;
        if (strcmp(key, "name") == 0) ok = json__string(json, station->name, sizeof(station->name));
        else if (strcmp(key, "url_resolved") == 0) ok = json__string(json, station->url, sizeof(station->url));
        else if (strcmp(key, "codec") == 0) ok = json__string(json, codec, sizeof(codec));
        else if (strcmp(key, "hls") == 0) ok = json__number(json, &hls);
        else ok = json__skip_value(json);
        if (!ok) return false;
    } while (json__eat(json, ','));
    if (!json__eat(json, '}')) return false;
    finder__codec(codec, station->codec);
    bool playable = strncmp(station->url, "http://", 7) == 0 || strncmp(station->url, "https://", 8) == 0;
    return playable && hls == 0 && station->name[0] != '\0';
}

size_t finder__parse(const char *text, size_t length, station_t *out, size_t max) {
    json_t json = {.p = text, .end = text + length};
    size_t count = 0;
    if (!json__eat(&json, '[') || json__eat(&json, ']')) return 0;
    do {
        station_t station;
        const char *start = json.p;
        if (finder__parse_station(&json, &station)) {
            if (count < max) out[count++] = station;
        } else {
            json.p = start;
            if (!json__skip_value(&json)) break;
        }
    } while (json__eat(&json, ','));
    return count;
}

static bruce_result_t finder__request(finder_t *finder) {
    static const char *const headers[] = {"User-Agent", FINDER_USER_AGENT};
    bruce_http_request_t request = {
        .url = finder->url,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = FINDER_TIMEOUT_MS,
    };
    bruce_result_t result = http__request_start(&request, &finder->job);
    finder->state = result == BRUCE_OK ? FINDER_RUNNING : FINDER_IDLE;
    return result;
}

bruce_result_t finder__start(finder_t *finder, const char *base, const char *query) {
    finder__cancel(finder);
    if (!finder__build_url(base, query, finder->url, sizeof(finder->url))) return BRUCE_ERR_INVALID_ARGUMENT;
    if (net__is_online()) return finder__request(finder);
    bruce_result_t result = net__connect_start();
    if (result == BRUCE_OK) finder->state = FINDER_CONNECTING;
    return result;
}

bruce_result_t finder__poll(finder_t *finder, stations_t *out) {
    out->count = 0;
    if (finder->state == FINDER_CONNECTING) {
        bruce_result_t result = net__connect_poll();
        if (result == BRUCE_ERR_BUSY) return result;
        if (result != BRUCE_OK) {
            finder->state = FINDER_IDLE;
            return result;
        }
        result = finder__request(finder);
        return result == BRUCE_OK ? BRUCE_ERR_BUSY : result;
    }
    if (finder->state != FINDER_RUNNING) return BRUCE_ERR_INVALID_STATE;
    bruce_http_response_t response;
    bruce_result_t result = http__request_poll(finder->job, &response);
    if (result == BRUCE_ERR_BUSY) return result;
    finder->state = FINDER_IDLE;
    finder->job = BRUCE_HTTP_JOB_ID_INVALID;
    if (result != BRUCE_OK) return result;
    if (response.status_code == 200) {
        out->count = finder__parse(response.body, response.body_len, out->items, FINDER_RESULTS_MAX);
    } else {
        result = BRUCE_ERR_IO;
    }
    http__response_free(&response);
    return result;
}

void finder__cancel(finder_t *finder) {
    if (finder->state == FINDER_RUNNING) http__request_cancel(finder->job);
    finder->state = FINDER_IDLE;
    finder->job = BRUCE_HTTP_JOB_ID_INVALID;
}
