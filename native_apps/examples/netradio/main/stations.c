#include "stations.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

#define STATIONS_FILE_MAX (24u * 1024u)

static void stations__copy(char *out, size_t capacity, const char *text, size_t length) {
    if (length >= capacity) length = capacity - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

/* `codec="..."` among an #EXTINF line's attributes, if any. */
static void stations__codec_attribute(const char *line, size_t length, char *out) {
    out[0] = '\0';
    const char *key = "codec=\"";
    for (size_t i = 0; i + 7 <= length; ++i) {
        if (strncmp(line + i, key, 7) != 0) continue;
        const char *start = line + i + 7;
        const char *end = memchr(start, '"', length - i - 7);
        if (end != NULL) stations__copy(out, STATION_CODEC_MAX, start, (size_t)(end - start));
        return;
    }
}

void stations__parse(const char *text, size_t length, stations_t *out) {
    memset(out, 0, sizeof(*out));
    station_t pending = {0};
    bool named = false;
    size_t at = 0;
    while (at < length && out->count < STATIONS_MAX) {
        size_t end = at;
        while (end < length && text[end] != '\n') ++end;
        size_t line_length = end - at;
        while (line_length > 0 && (text[at + line_length - 1] == '\r' || text[at + line_length - 1] == ' ')) {
            --line_length;
        }
        const char *line = text + at;
        if (line_length > 8 && strncmp(line, "#EXTINF:", 8) == 0) {
            const char *comma = memchr(line, ',', line_length);
            memset(&pending, 0, sizeof(pending));
            if (comma != NULL) {
                stations__copy(pending.name, sizeof(pending.name), comma + 1, line_length - (size_t)(comma + 1 - line));
                stations__codec_attribute(line, (size_t)(comma - line), pending.codec);
            }
            named = true;
        } else if (line_length > 0 && line[0] != '#') {
            stations__copy(pending.url, sizeof(pending.url), line, line_length);
            if (!named || pending.name[0] == '\0') {
                stations__copy(pending.name, sizeof(pending.name), pending.url, strlen(pending.url));
            }
            if (strncmp(pending.url, "http://", 7) == 0 || strncmp(pending.url, "https://", 8) == 0) {
                out->items[out->count++] = pending;
            }
            memset(&pending, 0, sizeof(pending));
            named = false;
        }
        at = end + 1;
    }
}

bool stations__format(const stations_t *stations, char *out, size_t capacity) {
    size_t used = 0;
    int written = snprintf(out, capacity, "#EXTM3U\n");
    if (written < 0 || (size_t)written >= capacity) return false;
    used = (size_t)written;
    for (size_t i = 0; i < stations->count; ++i) {
        const station_t *station = &stations->items[i];
        if (station->codec[0] != '\0') {
            written = snprintf(out + used, capacity - used, "#EXTINF:-1 codec=\"%s\",%s\n%s\n", station->codec,
                               station->name, station->url);
        } else {
            written = snprintf(out + used, capacity - used, "#EXTINF:-1,%s\n%s\n", station->name, station->url);
        }
        if (written < 0 || (size_t)written >= capacity - used) return false;
        used += (size_t)written;
    }
    return true;
}

bool stations__add(stations_t *stations, const station_t *station) {
    if (stations->count >= STATIONS_MAX) return false;
    for (size_t i = 0; i < stations->count; ++i) {
        if (strcmp(stations->items[i].url, station->url) == 0) return false;
    }
    stations->items[stations->count++] = *station;
    return true;
}

void stations__remove(stations_t *stations, size_t index) {
    if (index >= stations->count) return;
    memmove(&stations->items[index], &stations->items[index + 1],
            (stations->count - index - 1) * sizeof(stations->items[0]));
    stations->count--;
}

void stations__defaults(stations_t *out) {
    static const station_t defaults[] = {
        {"SomaFM Groove Salad", "https://ice1.somafm.com/groovesalad-128-mp3", "mp3"},
        {"SomaFM Drone Zone", "https://ice1.somafm.com/dronezone-128-mp3", "mp3"},
        {"SomaFM Secret Agent", "https://ice1.somafm.com/secretagent-128-mp3", "mp3"},
        {"Radio Paradise", "https://stream.radioparadise.com/mp3-128", "mp3"},
    };
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) out->items[out->count++] = defaults[i];
}

static bruce_result_t stations__read_file(const char *path, char *buffer, size_t capacity, size_t *out_length) {
    bruce_file_id_t file;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    size_t total = 0;
    while (total < capacity) {
        size_t got = 0;
        result = storage__read(file, buffer + total, capacity - total, &got);
        if (result != BRUCE_OK || got == 0) break;
        total += got;
    }
    storage__close(file);
    *out_length = total;
    return result;
}

bruce_result_t stations__load(const char *path, stations_t *out) {
    bool exists = false;
    if (storage__exists(path, &exists) != BRUCE_OK || !exists) {
        stations__defaults(out);
        (void)stations__save(path, out);
        return BRUCE_OK;
    }
    char *text = memory__malloc(STATIONS_FILE_MAX);
    if (text == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t length = 0;
    bruce_result_t result = stations__read_file(path, text, STATIONS_FILE_MAX, &length);
    if (result == BRUCE_OK) stations__parse(text, length, out);
    memory__free(text);
    return result;
}

/* The folder is made if missing, and the file replaced whole. */
bruce_result_t stations__save(const char *path, const stations_t *stations) {
    char *text = memory__malloc(STATIONS_FILE_MAX);
    if (text == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = BRUCE_ERR_RESOURCE_LIMIT;
    if (stations__format(stations, text, STATIONS_FILE_MAX)) {
        char folder[128];
        const char *slash = strrchr(path, '/');
        if (slash != NULL && slash != path) {
            snprintf(folder, sizeof(folder), "%.*s", (int)(slash - path), path);
            (void)storage__mkdir(folder);
        }
        bruce_file_id_t file;
        result = storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
                               &file);
        if (result == BRUCE_OK) {
            size_t written = 0;
            result = storage__write(file, text, strlen(text), &written);
            storage__close(file);
        }
    }
    memory__free(text);
    return result;
}
