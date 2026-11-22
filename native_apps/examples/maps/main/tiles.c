#include "tiles.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/http.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"
#include "core_sdk/net.h"
#include "core_sdk/display.h"
#include "geo.h"
#include "ink.h"

#define TILES_BYTES (GEO_TILE_SIZE * GEO_TILE_SIZE * 2)
#define TILES_MAX_SLOTS 16
#define TILES_SWAP_SLOTS 6
#define TILES_DOWNLOAD_MAX_BYTES (256u * 1024u)
#define TILES_DOWNLOAD_TIMEOUT_MS 15000
#define TILES_NETWORK_CHECK_MS 1000
#define TILES_WIFI_RETRY_MS 30000
#define TILES_USER_AGENT "BruceOS-Maps/0.1"

static bool tiles__same(tile_key_t a, tile_key_t b) { return a.z == b.z && a.x == b.x && a.y == b.y; }

/* PSRAM holds plenty of decoded tiles; without it they land in flash swap, so keep few. */
static size_t tiles__slot_count(size_t min_slots) {
    bruce_memory_stats_t stats = {0};
    size_t count = TILES_SWAP_SLOTS;
    if (memory__get_stats(&stats) == BRUCE_OK && stats.psram_free > 0) {
        count = stats.psram_free / 2 / TILES_BYTES;
        if (count > TILES_MAX_SLOTS) count = TILES_MAX_SLOTS;
    }
    return count < min_slots ? min_slots : count;
}

bool tiles__init(tiles_t *tiles, size_t min_slots, const char *dir, const char *url, bool online) {
    memset(tiles, 0, sizeof(*tiles));
    tiles->slot_count = tiles__slot_count(min_slots);
    tiles->slots = memory__calloc(tiles->slot_count, sizeof(tile_slot_t));
    tiles->dir = dir;
    tiles->url = url;
    tiles->online = online;
    return tiles->slots != NULL;
}

static void tiles__cancel_downloads(tiles_t *tiles) {
    for (size_t i = 0; i < tiles->download_count; ++i) http__request_cancel(tiles->downloads[i].job);
    tiles->download_count = 0;
}

void tiles__deinit(tiles_t *tiles) {
    tiles__cancel_downloads(tiles);
    for (size_t i = 0; i < tiles->slot_count; ++i) image__bitmap_release(&tiles->slots[i].bitmap);
    memory__free(tiles->slots);
    tiles->slots = NULL;
}

static void tiles__forget_missing(tiles_t *tiles) {
    tiles->missing_count = 0;
    tiles->missing_next = 0;
}

void tiles__set_online(tiles_t *tiles, bool online) {
    tiles__cancel_downloads(tiles);
    tiles->online = online;
    tiles->connecting = false;
    tiles->wifi_retry_ms = 0;
    tiles->network_down = false;
    tiles__forget_missing(tiles);
}

static void tiles__network_back(tiles_t *tiles) {
    tiles->network_down = false;
    tiles__forget_missing(tiles);
}

bool tiles__check_network(tiles_t *tiles) {
    if (tiles->connecting) {
        bruce_result_t result = net__connect_poll();
        if (result == BRUCE_ERR_BUSY) return false;
        tiles->connecting = false;
        if (result == BRUCE_OK) tiles__network_back(tiles);
        else tiles->network_down = true;
        return true;
    }
    if (!tiles->network_down) return false;
    uint64_t now = runtime__now();
    if (now < tiles->network_checked_ms + TILES_NETWORK_CHECK_MS) return false;
    tiles->network_checked_ms = now;
    if (!net__is_online()) return false;
    tiles__network_back(tiles);
    return true;
}

const image_bitmap_t *tiles__get(tiles_t *tiles, tile_key_t key) {
    for (size_t i = 0; i < tiles->slot_count; ++i) {
        tile_slot_t *slot = &tiles->slots[i];
        if (slot->bitmap.pixels != NULL && tiles__same(slot->key, key)) {
            slot->last_used = ++tiles->clock;
            return &slot->bitmap;
        }
    }
    return NULL;
}

bool tiles__is_missing(const tiles_t *tiles, tile_key_t key) {
    for (size_t i = 0; i < tiles->missing_count; ++i) {
        if (tiles__same(tiles->missing[i], key)) return true;
    }
    return false;
}

static void tiles__mark_missing(tiles_t *tiles, tile_key_t key) {
    tiles->missing[tiles->missing_next] = key;
    tiles->missing_next = (tiles->missing_next + 1) % TILES_MISSING_MAX;
    if (tiles->missing_count < TILES_MISSING_MAX) tiles->missing_count++;
}

static tile_slot_t *tiles__oldest_slot(tiles_t *tiles) {
    tile_slot_t *oldest = &tiles->slots[0];
    for (size_t i = 0; i < tiles->slot_count; ++i) {
        tile_slot_t *slot = &tiles->slots[i];
        if (slot->bitmap.pixels == NULL) return slot;
        if (slot->last_used < oldest->last_used) oldest = slot;
    }
    return oldest;
}

static void tiles__path(const tiles_t *tiles, tile_key_t key, char *out, size_t capacity) {
    snprintf(out, capacity, "%s/%d/%d/%d.png", tiles->dir, key.z, key.x, key.y);
}

static void tiles__url(const tiles_t *tiles, tile_key_t key, char *out, size_t capacity) {
    size_t length = 0;
    for (const char *p = tiles->url; *p != '\0' && length + 1 < capacity; ++p) {
        int value = -1;
        if (strncmp(p, "{z}", 3) == 0) value = key.z;
        if (strncmp(p, "{x}", 3) == 0) value = key.x;
        if (strncmp(p, "{y}", 3) == 0) value = key.y;
        if (value < 0) {
            out[length++] = *p;
            continue;
        }
        int written = snprintf(out + length, capacity - length, "%d", value);
        if (written < 0 || (size_t)written >= capacity - length) break;
        length += (size_t)written;
        p += 2;
    }
    out[length] = '\0';
}

static void tiles__make_parent_dirs(const char *path) {
    char dir[BRUCE_STORAGE_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *slash = strchr(dir + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        (void)storage__mkdir(dir);
        *slash = '/';
    }
}

/* Uses the network when online; otherwise starts getting online (Wi-Fi or
 * mobile data) in the background, at most every 30 s, and reports not ready meanwhile. */
static bool tiles__network_ready(tiles_t *tiles) {
    if (!tiles->online) return false;
    if (net__is_online()) {
        if (tiles->network_down) tiles__network_back(tiles);
        return true;
    }
    if (!tiles->connecting && runtime__now() >= tiles->wifi_retry_ms) {
        tiles->wifi_retry_ms = runtime__now() + TILES_WIFI_RETRY_MS;
        tiles->connecting = net__connect_start() == BRUCE_OK;
        if (!tiles->connecting) tiles->network_down = true;
    }
    return false;
}

static bool tiles__is_downloading(const tiles_t *tiles, tile_key_t key) {
    for (size_t i = 0; i < tiles->download_count; ++i) {
        if (tiles__same(tiles->downloads[i].key, key)) return true;
    }
    return false;
}

bool tiles__needs_load(tiles_t *tiles, tile_key_t key) {
    if (tiles__is_missing(tiles, key) || tiles__is_downloading(tiles, key)) return false;
    return tiles__get(tiles, key) == NULL;
}

static bool tiles__decode(tiles_t *tiles, tile_key_t key, const char *path) {
    bruce_image_draw_options_t options = {.background = BRUCE_COLOR_WHITE};
    tile_slot_t *slot = tiles__oldest_slot(tiles);
    image__bitmap_release(&slot->bitmap);
    if (image__get_bitmap_from_file(path, &options, &slot->bitmap) != BRUCE_OK) return false;
    if (slot->bitmap.width != GEO_TILE_SIZE || slot->bitmap.height != GEO_TILE_SIZE) {
        image__bitmap_release(&slot->bitmap);
        return false;
    }
    if (display__is_epaper()) ink__convert(slot->bitmap.pixels, GEO_TILE_SIZE, GEO_TILE_SIZE);
    slot->key = key;
    slot->last_used = ++tiles->clock;
    return true;
}

static tile_load_t tiles__start_download(tiles_t *tiles, tile_key_t key) {
    if (tiles->download_count >= TILES_DOWNLOADS_MAX) return TILE_DEFERRED;
    char url[192];
    tiles__url(tiles, key, url, sizeof(url));
    static const char *const headers[] = {"User-Agent", TILES_USER_AGENT};
    bruce_http_request_t request = {
        .url = url,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = TILES_DOWNLOAD_TIMEOUT_MS,
        .max_response_bytes = TILES_DOWNLOAD_MAX_BYTES,
    };
    bruce_http_job_id_t job = BRUCE_HTTP_JOB_ID_INVALID;
    bruce_result_t result = http__request_start(&request, &job);
    if (result == BRUCE_ERR_RESOURCE_LIMIT) return TILE_DEFERRED;
    if (result != BRUCE_OK) {
        tiles__mark_missing(tiles, key);
        return TILE_MISSING;
    }
    tiles->downloads[tiles->download_count++] = (tile_download_t){.key = key, .job = job};
    return TILE_DOWNLOADING;
}

tile_load_t tiles__load(tiles_t *tiles, tile_key_t key) {
    char path[BRUCE_STORAGE_PATH_MAX];
    tiles__path(tiles, key, path, sizeof(path));
    bool exists = false;
    (void)storage__exists(path, &exists);
    if (exists && tiles__decode(tiles, key, path)) return TILE_LOADED;
    if (!exists && tiles__network_ready(tiles)) return tiles__start_download(tiles, key);
    tiles__mark_missing(tiles, key);
    return TILE_MISSING;
}

/* Written to a .part file first so a half-written tile never looks complete. */
static bool tiles__save(const tiles_t *tiles, tile_key_t key, const bruce_http_response_t *response) {
    char path[BRUCE_STORAGE_PATH_MAX];
    char part_path[BRUCE_STORAGE_PATH_MAX + sizeof(".part")];
    tiles__path(tiles, key, path, sizeof(path));
    snprintf(part_path, sizeof(part_path), "%s.part", path);
    tiles__make_parent_dirs(path);

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    uint32_t flags = BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE;
    if (storage__open(part_path, flags, &file) != BRUCE_OK) return false;
    size_t written = 0;
    bool ok = storage__write(file, response->body, response->body_len, &written) == BRUCE_OK &&
              written == response->body_len;
    ok = storage__close(file) == BRUCE_OK && ok;
    ok = ok && storage__rename(part_path, path) == BRUCE_OK;
    if (!ok) (void)storage__remove(part_path);
    return ok;
}

static void
tiles__finish(tiles_t *tiles, tile_key_t key, bruce_result_t result, const bruce_http_response_t *response) {
    bool ok = result == BRUCE_OK && response->status_code == 200 && response->body_len > 0 &&
              tiles__save(tiles, key, response);
    if (!ok) tiles__mark_missing(tiles, key);
    if (result == BRUCE_ERR_IO && !net__is_online()) tiles->network_down = true;
}

bool tiles__poll(tiles_t *tiles) {
    bool finished = false;
    for (size_t i = 0; i < tiles->download_count;) {
        tile_download_t download = tiles->downloads[i];
        bruce_http_response_t response = {0};
        bruce_result_t result = http__request_poll(download.job, &response);
        if (result == BRUCE_ERR_BUSY) {
            ++i;
            continue;
        }
        tiles__finish(tiles, download.key, result, &response);
        http__response_free(&response);
        tiles->downloads[i] = tiles->downloads[--tiles->download_count];
        finished = true;
    }
    return finished;
}

bool tiles__cancel_unless(tiles_t *tiles, bool (*keep)(tile_key_t key, void *context), void *context) {
    size_t before = tiles->download_count;
    for (size_t i = 0; i < tiles->download_count;) {
        if (keep(tiles->downloads[i].key, context)) {
            ++i;
            continue;
        }
        http__request_cancel(tiles->downloads[i].job);
        tiles->downloads[i] = tiles->downloads[--tiles->download_count];
    }
    return tiles->download_count != before;
}
