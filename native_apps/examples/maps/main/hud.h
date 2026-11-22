#pragma once

#include <stdbool.h>

#include "map_view.h"
#include "route.h"

/* What Prev/Next do. */
typedef enum {
    HUD_NAV_ZOOM,
    HUD_NAV_UP_DOWN,
    HUD_NAV_LEFT_RIGHT,
    HUD_NAV_COUNT,
} hud_nav_mode_t;

typedef enum {
    HUD_BUTTON_ZOOM_IN,
    HUD_BUTTON_ZOOM_OUT,
    HUD_BUTTON_LOCATE,
} hud_button_t;

typedef struct {
    int x, y, w, h;
} hud_rect_t;

typedef struct {
    const char *status;
    const char *status_icon;
    hud_nav_mode_t nav_mode;
    const char *hint[2]; /* Short-lived message centred near the bottom. */
    const char *hint_icon;
    bool pin;            /* Marker for the place found by search. */
    double pin_lat;
    double pin_lon;
    const char *place;   /* Pinned place's name, shown in the search box. */
    bool zoom_buttons;   /* On-screen +/- for touch screens. */
    bool me;             /* GPS position. */
    double me_lat;
    double me_lon;
    bool locate_button;  /* "My location" button, bottom right. */
    bool following;      /* The map follows the GPS position. */
    const char *gps_status; /* Progress towards a fix, under the status. */
    bool speed;
    int speed_kmh;
    const route_t *route;            /* Drawn under the markers. */
    const route_step_t *banner_step; /* The banner at the top shows while set, instead of search. */
    const char *banner_distance;     /* Large, above the instruction. */
    const char *banner_text;
    const char *banner_then;         /* The manoeuvre after, under the banner. */
    const char *summary;             /* Distance and time left, bottom centre. */
} hud_state_t;

const char *hud__nav_icon(hud_nav_mode_t mode);
hud_rect_t hud__button_rect(hud_button_t button, int width, int height);
hud_rect_t hud__search_rect(const map_view_t *view, const hud_state_t *state, int width);
hud_rect_t hud__banner_rect(int width);

void hud__draw(const map_view_t *view, const hud_state_t *state, int width, int height);
