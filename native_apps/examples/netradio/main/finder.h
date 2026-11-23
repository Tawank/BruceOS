#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/http.h"
#include "core_sdk/result.h"
#include "stations.h"

#define FINDER_QUERY_MAX 48
#define FINDER_RESULTS_MAX 20

/* Searches the public Radio Browser directory (radio-browser.info) by name. */
typedef enum {
    FINDER_IDLE,
    FINDER_CONNECTING, /* Waiting for the background net__connect_start(). */
    FINDER_RUNNING,
} finder_state_t;

typedef struct {
    finder_state_t state;
    bruce_http_job_id_t job;
    char url[256];
} finder_t;

bool finder__build_url(const char *base, const char *query, char *out, size_t capacity);

/* Radio Browser's JSON station array, most played first. HLS streams are
 * skipped (they can't be played as one stream). Returns how many were read. */
size_t finder__parse(const char *json, size_t length, station_t *out, size_t max);

bruce_result_t finder__start(finder_t *finder, const char *base, const char *query);
/* BRUCE_ERR_BUSY while running; otherwise the result, with the stations on BRUCE_OK. */
bruce_result_t finder__poll(finder_t *finder, stations_t *out);
void finder__cancel(finder_t *finder);
