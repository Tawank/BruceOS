#include "tiles.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/http.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"
#include "core_sdk/wifi.h"
#include "geo.h"

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

void tiles__deinit(tiles_t *tiles) {
    for (size_t i = 0; i < tiles->slot_count; ++i) image__bitmap_release(&tiles->slots[i].bitmap);
    memory__free(tiles->slots);
    tiles->slots = NULL;
}

static void tiles__forget_missing(tiles_t *tiles) {
    tiles->missing_count = 0;
    tiles->missing_next = 0;
}

void tiles__set_online(tiles_t *tiles, bool online) {
    tiles->online = online;
    tiles->network_down = false;
    tiles__forget_missing(tiles);
}

static void tiles__network_back(tiles_t *tiles) {
    tiles->network_down = false;
    tiles__forget_missing(tiles);
}

bool tiles__check_network(tiles_t *tiles) {
    if (!tiles->network_down) return false;
    uint64_t now = runtime__now();
    if (now < tiles->network_checked_ms + TILES_NETWORK_CHECK_MS) return false;
    tiles->network_checked_ms = now;
    if (!wifi__is_connected()) return false;
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

/* Uses Wi-Fi when it is up; otherwise tries the saved network, at most every 30 s. */
static bool tiles__network_ready(tiles_t *tiles) {
    if (!tiles->online) return false;
    if (!wifi__is_connected()) {
        if (tiles->network_down && runtime__now() < tiles->wifi_retry_ms) return false;
        tiles->wifi_retry_ms = runtime__now() + TILES_WIFI_RETRY_MS;
        if (wifi__connect_known() != BRUCE_OK || !wifi__is_connected()) {
            tiles->network_down = true;
            return false;
        }
    }
    if (tiles->network_down) tiles__network_back(tiles);
    return true;
}

static bruce_result_t tiles__write_chunk(const void *data, size_t size, void *context) {
    size_t written = 0;
    bruce_result_t result = storage__write(*(bruce_file_id_t *)context, data, size, &written);
    return result == BRUCE_OK && written == size ? BRUCE_OK : BRUCE_ERR_IO;
}

static bool tiles__download(tiles_t *tiles, tile_key_t key, const char *path) {
    char url[192];
    char part_path[BRUCE_STORAGE_PATH_MAX];
    tiles__url(tiles, key, url, sizeof(url));
    snprintf(part_path, sizeof(part_path), "%s.part", path);
    tiles__make_parent_dirs(path);

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    uint32_t flags = BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE;
    if (storage__open(part_path, flags, &file) != BRUCE_OK) return false;

    static const char *const headers[] = {"User-Agent", TILES_USER_AGENT};
    bruce_http_request_t request = {
        .url = url,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = TILES_DOWNLOAD_TIMEOUT_MS,
        .max_response_bytes = TILES_DOWNLOAD_MAX_BYTES,
        .on_response_chunk = tiles__write_chunk,
        .response_chunk_context = &file,
    };
    bruce_http_response_t response = {0};
    bool ok = http__request(&request, &response) == BRUCE_OK && response.status_code == 200 &&
              response.body_len > 0;
    http__response_free(&response);
    ok = storage__close(file) == BRUCE_OK && ok;
    ok = ok && storage__rename(part_path, path) == BRUCE_OK;
    if (!ok) (void)storage__remove(part_path);
    return ok;
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
    slot->key = key;
    slot->last_used = ++tiles->clock;
    return true;
}

void tiles__load(tiles_t *tiles, tile_key_t key) {
    char path[BRUCE_STORAGE_PATH_MAX];
    tiles__path(tiles, key, path, sizeof(path));

    bool exists = false;
    (void)storage__exists(path, &exists);
    if (!exists && tiles__network_ready(tiles)) exists = tiles__download(tiles, key, path);
    if (!exists || !tiles__decode(tiles, key, path)) tiles__mark_missing(tiles, key);
}
