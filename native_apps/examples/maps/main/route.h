#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/http.h"
#include "core_sdk/result.h"
#include "geo.h"

/* Routes from an OSRM server (by default FOSSGIS's, which has car, bike and
 * foot profiles), with turn-by-turn steps. */

#define ROUTE_ZOOM GEO_MAX_ZOOM
#define ROUTE_NAME_MAX 40
#define ROUTE_TEXT_MAX 96

typedef enum {
    ROUTE_CAR,
    ROUTE_BIKE,
    ROUTE_FOOT,
    ROUTE_PROFILE_COUNT,
} route_profile_t;

typedef enum {
    ROUTE_DEPART,
    ROUTE_ARRIVE,
    ROUTE_TURN,
    ROUTE_CONTINUE,
    ROUTE_MERGE,
    ROUTE_ON_RAMP,
    ROUTE_OFF_RAMP,
    ROUTE_FORK,
    ROUTE_END_OF_ROAD,
    ROUTE_ROUNDABOUT,
} route_turn_t;

typedef struct {
    route_turn_t turn;
    int16_t angle;    /* Degrees: 0 straight on, negative left, positive right, 180 U-turn. */
    uint16_t bearing; /* Direction after the manoeuvre, 0 = north. */
    uint8_t exit;     /* Roundabout exit, 0 when not given. */
    uint32_t point;   /* The manoeuvre's index in route_t.points. */
    float duration_s; /* From this manoeuvre to the next. */
    char name[ROUTE_NAME_MAX];
} route_step_t;

typedef struct {
    geo_point_t *points; /* At ROUTE_ZOOM. */
    float *along;        /* Metres from the start to each point. */
    size_t point_count;
    route_step_t *steps;
    size_t step_count;
} route_t;

typedef enum {
    ROUTE_IDLE,
    ROUTE_CONNECTING, /* Waiting for the background net__connect_start(). */
    ROUTE_RUNNING,
} route_state_t;

typedef struct {
    route_state_t state;
    bruce_http_job_id_t job;
    char url[256];
} route_request_t;

/* Where a position is along a route. */
typedef struct {
    size_t segment;  /* Pass back to route__track() next time. */
    float off_m;     /* Distance from the route line. */
    size_t next;     /* Step of the next manoeuvre. */
    float to_next_m;
    float left_m;
    float left_s;
} route_progress_t;

bool route__build_url(const char *base, route_profile_t profile, double from_lat, double from_lon, double to_lat,
                      double to_lon, char *out, size_t capacity);

/* An OSRM route reply. BRUCE_ERR_NOT_FOUND when the server found no route. */
bruce_result_t route__parse(const char *json, size_t len, route_t *route);
void route__free(route_t *route);

float route__length_m(const route_t *route);
float route__duration_s(const route_t *route);

/* "Turn left onto Main Street". */
void route__instruction(const route_step_t *step, char *out, size_t capacity);
/* "350 m", "1.2 km", "12 km". */
void route__format_distance(char *out, size_t capacity, double meters);
/* Coarser steps further out, so the text changes less often. */
void route__format_countdown(char *out, size_t capacity, double meters);
/* "6 min", "1 h 05 min". */
void route__format_duration(char *out, size_t capacity, double seconds);

/* Finds the nearest point of the route at or after `from_segment` (less a few, for GPS jitter). */
void route__track(const route_t *route, double lat, double lon, size_t from_segment, route_progress_t *out);

/* Starts in the background, getting online first when needed. */
bruce_result_t route__start(route_request_t *request, const char *base, route_profile_t profile, double from_lat,
                            double from_lon, double to_lat, double to_lon);
/* BRUCE_ERR_BUSY while running; otherwise the result, with *out filled on BRUCE_OK. */
bruce_result_t route__poll(route_request_t *request, route_t *out);
void route__cancel(route_request_t *request);
