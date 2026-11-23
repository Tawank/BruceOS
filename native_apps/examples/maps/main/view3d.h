#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "hud.h"
#include "map_view.h"
#include "tiles.h"

#define VIEW3D_KEYS_MAX 24

/* The map as a tilted ground plane seen from behind the centre, turned so
 * the direction of travel is up (Mode 7 style). Uses tiles one zoom level
 * out, which keeps the ground ahead within the tile cache. */
typedef struct {
    bool on;
    float heading_deg; /* Up on screen; 0 = north. */
    tile_key_t keys[VIEW3D_KEYS_MAX]; /* Tiles on screen, nearest first, from the last view3d__pending(). */
    size_t key_count;
} view3d_t;

/* Returns true when the heading changed. */
bool view3d__set_heading(view3d_t *view3d, float heading_deg);

/* Turns the view by `degrees`, clockwise. */
void view3d__turn(view3d_t *view3d, float degrees);

/* A pan in screen pixels turned into map pixels for the heading. */
void view3d__rotate_pan(const view3d_t *view3d, int *dx, int *dy);

/* Like map_view__pending(), for the tilted view. */
size_t view3d__pending(view3d_t *view3d, const map_view_t *view, tiles_t *tiles, int width, int height,
                       tile_key_t *out_keys, size_t capacity);
bool view3d__is_near(const view3d_t *view3d, tile_key_t key);

/* Ground, route, pin and position; the HUD's flat markers are left to the caller to hide. */
void view3d__draw(const view3d_t *view3d, const map_view_t *view, tiles_t *tiles, const hud_state_t *hud, int width,
                  int height);
