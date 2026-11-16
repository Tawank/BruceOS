#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/http.h"
#include "core_sdk/image.h"

#define TILES_MISSING_MAX 64
#define TILES_DOWNLOADS_MAX 5

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
    tile_key_t key;
    bruce_http_job_id_t job;
} tile_download_t;

typedef enum {
    TILE_LOADED,      /* Decoded and cached. */
    TILE_DOWNLOADING, /* Fetching in the background; tiles__poll() reports when it lands. */
    TILE_DEFERRED,    /* Not on SD and every download slot is busy; try again later. */
    TILE_MISSING,     /* Not available; remembered so it is not retried right away. */
} tile_load_t;

typedef struct {
    tile_slot_t *slots;
    size_t slot_count;
    uint32_t clock;
    tile_key_t missing[TILES_MISSING_MAX];
    size_t missing_count;
    size_t missing_next;
    tile_download_t downloads[TILES_DOWNLOADS_MAX];
    size_t download_count;
    const char *dir;
    const char *url;
    bool online;
    bool connecting; /* Background Wi-Fi connect in progress. */
    bool network_down;
    uint64_t network_checked_ms;
    uint64_t wifi_retry_ms;
} tiles_t;

bool tiles__init(tiles_t *tiles, size_t min_slots, const char *dir, const char *url, bool online);
void tiles__deinit(tiles_t *tiles);
void tiles__set_online(tiles_t *tiles, bool online);

/* Follows the background Wi-Fi connect and notices Wi-Fi coming back (checked at most once a
 * second), retrying missing tiles then. Returns true when the network state changed. */
bool tiles__check_network(tiles_t *tiles);

/* Cached tile or NULL; never touches storage or network. */
const image_bitmap_t *tiles__get(tiles_t *tiles, tile_key_t key);
bool tiles__is_missing(const tiles_t *tiles, tile_key_t key);

/* Not cached, not missing and not downloading: worth a tiles__load(). */
bool tiles__needs_load(tiles_t *tiles, tile_key_t key);

/* Decodes from SD, or starts a background download into the SD cache. */
tile_load_t tiles__load(tiles_t *tiles, tile_key_t key);

/* Stores finished downloads on SD. Returns true when any finished. */
bool tiles__poll(tiles_t *tiles);

/* Cancels downloads for tiles `keep` no longer wants, e.g. after fast scrolling. Returns true if any. */
bool tiles__cancel_unless(tiles_t *tiles, bool (*keep)(tile_key_t key, void *context), void *context);
