#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/http.h"
#include "core_sdk/result.h"
#include "map_view.h"

#define SEARCH_RESULTS_MAX 8
#define SEARCH_NAME_MAX 96
#define SEARCH_QUERY_MAX 64

typedef struct {
    char name[SEARCH_NAME_MAX];
    double lat;
    double lon;
} search_place_t;

typedef enum {
    SEARCH_IDLE,
    SEARCH_CONNECTING, /* Waiting for the background Wi-Fi connect. */
    SEARCH_RUNNING,
} search_state_t;

typedef struct {
    search_state_t state;
    bruce_http_job_id_t job;
    char url[384];
} search_t;

/* "50.06, 19.94" or "50.06 19.94". */
bool search__parse_coordinates(const char *text, double *out_lat, double *out_lon);

/* Nominatim jsonv2 results. Returns how many places were read. */
size_t search__parse(const char *json, size_t len, search_place_t *places, size_t max);

/* Places near what is on screen rank first. False when it does not fit. */
bool search__build_url(
    const char *base, const char *query, const map_view_t *view, int width, int height, char *out, size_t capacity
);

/* Starts in the background, connecting Wi-Fi first when needed. */
bruce_result_t search__start(search_t *search, const char *base, const char *query, const map_view_t *view,
                             int width, int height);

/* BRUCE_ERR_BUSY while running; otherwise the result, with the places on BRUCE_OK. */
bruce_result_t search__poll(search_t *search, search_place_t *places, size_t *out_count);

void search__cancel(search_t *search);
