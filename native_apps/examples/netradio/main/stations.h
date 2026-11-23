#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/audio.h"
#include "core_sdk/result.h"

#define STATIONS_MAX 32
#define STATION_NAME_MAX 64
#define STATION_CODEC_MAX 8

typedef struct {
    char name[STATION_NAME_MAX];
    char url[BRUCE_AUDIO_URL_MAX];
    char codec[STATION_CODEC_MAX]; /* "mp3", "aac", ... or "" to go by the URL */
} station_t;

typedef struct {
    station_t items[STATIONS_MAX];
    size_t count;
} stations_t;

/* Saved stations live in an M3U file anyone can edit:
 *   #EXTM3U
 *   #EXTINF:-1 codec="aac",Station name
 *   https://example.com/stream
 * Unknown lines are skipped. */
void stations__parse(const char *text, size_t length, stations_t *out);
/* Writes `stations` as M3U into `out` (NUL-terminated); false when it doesn't fit. */
bool stations__format(const stations_t *stations, char *out, size_t capacity);

/* Adds unless a station with the same URL is there; false when full or a duplicate. */
bool stations__add(stations_t *stations, const station_t *station);
void stations__remove(stations_t *stations, size_t index);
/* A few stations to start with. */
void stations__defaults(stations_t *out);

/* Loads `path`; a missing file gives the defaults (and writes them). */
bruce_result_t stations__load(const char *path, stations_t *out);
bruce_result_t stations__save(const char *path, const stations_t *stations);
