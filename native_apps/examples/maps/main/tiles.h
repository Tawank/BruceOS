#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/image.h"

#define TILES_MISSING_MAX 64

typedef struct {
    int z;
    int x;
    int y;
} tile_key_t;

typedef struct {
    tile_key_t key;
    image_bitmap_t bitmap;
    uint32_t last_used;
} tile_slot_t;

typedef struct {
    tile_slot_t *slots;
    size_t slot_count;
    uint32_t clock;
    tile_key_t missing[TILES_MISSING_MAX];
    size_t missing_count;
    size_t missing_next;
    const char *dir;
    const char *url;
    bool online;
    bool network_down;
    uint64_t network_checked_ms;
    uint64_t wifi_retry_ms;
} tiles_t;

bool tiles__init(tiles_t *tiles, size_t min_slots, const char *dir, const char *url, bool online);
void tiles__deinit(tiles_t *tiles);
void tiles__set_online(tiles_t *tiles, bool online);

/* Notices Wi-Fi coming back (at most once a second) and retries missing tiles. Returns true then. */
bool tiles__check_network(tiles_t *tiles);

/* Cached tile or NULL; never touches storage or network. */
const image_bitmap_t *tiles__get(tiles_t *tiles, tile_key_t key);
bool tiles__is_missing(const tiles_t *tiles, tile_key_t key);

/* Loads from SD, downloading into the SD cache first when online. Blocks. */
void tiles__load(tiles_t *tiles, tile_key_t key);
