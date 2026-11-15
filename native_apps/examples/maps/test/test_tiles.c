#include "fakes.h"
#include "test.h"
#include "tiles.h"

#define DIR "/sdcard/maps/tiles"
#define URL "https://tiles.test/{z}/{x}/{y}.png?key=abc"
#define TILE_BYTES (256 * 256 * 2)

static tiles_t tiles;

static void init(bool online) { CHECK(tiles__init(&tiles, 6, DIR, URL, online)); }

static void slot_count_follows_psram(void) {
    init(false);
    CHECK_EQ(tiles.slot_count, 6u);
    tiles__deinit(&tiles);

    fakes.psram_free = 64 * TILE_BYTES;
    init(false);
    CHECK_EQ(tiles.slot_count, 16u);
    tiles__deinit(&tiles);

    fakes.psram_free = 2 * TILE_BYTES;
    CHECK(tiles__init(&tiles, 3, DIR, URL, false));
    CHECK_EQ(tiles.slot_count, 3u);
    tiles__deinit(&tiles);
}

static void loads_tile_from_sd_without_network(void) {
    fakes__add_file(DIR "/3/4/2.png", 500);
    init(true);
    tile_key_t key = {3, 4, 2};
    CHECK(tiles__get(&tiles, key) == NULL);
    tiles__load(&tiles, key);
    const image_bitmap_t *bitmap = tiles__get(&tiles, key);
    CHECK(bitmap != NULL);
    CHECK_EQ(fakes.http_calls, 0);
    CHECK_EQ(fakes.decode_count, 1);
    CHECK(!tiles__is_missing(&tiles, key));
    tiles__deinit(&tiles);
}

static void offline_missing_tile_is_remembered(void) {
    init(false);
    tile_key_t key = {3, 4, 2};
    tiles__load(&tiles, key);
    CHECK(tiles__get(&tiles, key) == NULL);
    CHECK(tiles__is_missing(&tiles, key));
    CHECK_EQ(fakes.http_calls, 0);
    CHECK_EQ(fakes.wifi_connect_calls, 0);
    tiles__deinit(&tiles);
}

static void online_download_goes_to_sd_cache(void) {
    init(true);
    tile_key_t key = {12, 2271, 1388};
    tiles__load(&tiles, key);
    CHECK_STR(fakes.http_url, "https://tiles.test/12/2271/1388.png?key=abc");
    CHECK_STR(fakes.http_user_agent, "BruceOS-Maps/0.1");
    CHECK(fakes__file_exists(DIR "/12/2271/1388.png"));
    CHECK(!fakes__file_exists(DIR "/12/2271/1388.png.part"));
    CHECK(fakes.mkdir_count >= 5);
    CHECK(tiles__get(&tiles, key) != NULL);
    tiles__deinit(&tiles);
}

static void cached_download_is_not_fetched_again(void) {
    init(true);
    tile_key_t key = {5, 1, 1};
    tiles__load(&tiles, key);
    tiles__deinit(&tiles);
    init(true);
    tiles__load(&tiles, key);
    CHECK_EQ(fakes.http_calls, 1);
    CHECK(tiles__get(&tiles, key) != NULL);
    tiles__deinit(&tiles);
}

static void http_error_leaves_no_file(void) {
    fakes.http_status = 404;
    init(true);
    tile_key_t key = {5, 1, 1};
    tiles__load(&tiles, key);
    CHECK(!fakes__file_exists(DIR "/5/1/1.png"));
    CHECK(!fakes__file_exists(DIR "/5/1/1.png.part"));
    CHECK(tiles__is_missing(&tiles, key));

    fakes.http_status = 200;
    fakes.http_result = BRUCE_ERR_TIMEOUT;
    tile_key_t other = {5, 2, 1};
    tiles__load(&tiles, other);
    CHECK(!fakes__file_exists(DIR "/5/2/1.png.part"));
    CHECK(tiles__is_missing(&tiles, other));
    tiles__deinit(&tiles);
}

static void empty_response_is_rejected(void) {
    fakes.http_body_len = 0;
    init(true);
    tiles__load(&tiles, (tile_key_t){5, 1, 1});
    CHECK(!fakes__file_exists(DIR "/5/1/1.png"));
    tiles__deinit(&tiles);
}

static void connects_wifi_once_and_gives_up(void) {
    fakes.wifi_connected = false;
    fakes.wifi_connect_ok = false;
    init(true);
    tiles__load(&tiles, (tile_key_t){5, 1, 1});
    tiles__load(&tiles, (tile_key_t){5, 2, 1});
    CHECK_EQ(fakes.wifi_connect_calls, 1);
    CHECK_EQ(fakes.http_calls, 0);
    CHECK(tiles.network_down);
    tiles__deinit(&tiles);
}

static void connects_wifi_when_needed(void) {
    fakes.wifi_connected = false;
    init(true);
    tiles__load(&tiles, (tile_key_t){5, 1, 1});
    CHECK_EQ(fakes.wifi_connect_calls, 1);
    CHECK_EQ(fakes.http_calls, 1);
    CHECK(!tiles.network_down);
    tiles__deinit(&tiles);
}

static void wrong_tile_size_is_rejected(void) {
    fakes.decode_size = 128;
    fakes__add_file(DIR "/3/4/2.png", 500);
    init(false);
    tile_key_t key = {3, 4, 2};
    tiles__load(&tiles, key);
    CHECK(tiles__get(&tiles, key) == NULL);
    CHECK(tiles__is_missing(&tiles, key));
    tiles__deinit(&tiles);
}

static void evicts_least_recently_used(void) {
    fakes.psram_free = 2 * TILE_BYTES;
    CHECK(tiles__init(&tiles, 2, DIR, URL, true));
    tile_key_t a = {5, 1, 1}, b = {5, 2, 1}, c = {5, 3, 1};
    tiles__load(&tiles, a);
    tiles__load(&tiles, b);
    CHECK(tiles__get(&tiles, a) != NULL);
    tiles__load(&tiles, c);
    CHECK(tiles__get(&tiles, a) != NULL);
    CHECK(tiles__get(&tiles, b) == NULL);
    CHECK(tiles__get(&tiles, c) != NULL);
    tiles__deinit(&tiles);
}

static void missing_list_keeps_newest(void) {
    init(false);
    for (int x = 0; x <= TILES_MISSING_MAX; ++x) tiles__load(&tiles, (tile_key_t){10, x, 0});
    CHECK_EQ(tiles.missing_count, (size_t)TILES_MISSING_MAX);
    CHECK(!tiles__is_missing(&tiles, (tile_key_t){10, 0, 0}));
    CHECK(tiles__is_missing(&tiles, (tile_key_t){10, 1, 0}));
    CHECK(tiles__is_missing(&tiles, (tile_key_t){10, TILES_MISSING_MAX, 0}));
    tiles__deinit(&tiles);
}

static void going_online_retries_missing(void) {
    fakes.wifi_connected = false;
    fakes.wifi_connect_ok = false;
    init(true);
    tile_key_t key = {5, 1, 1};
    tiles__load(&tiles, key);
    CHECK(tiles__is_missing(&tiles, key));

    tiles__set_online(&tiles, true);
    CHECK(!tiles.network_down);
    CHECK(!tiles__is_missing(&tiles, key));
    fakes.wifi_connect_ok = true;
    tiles__load(&tiles, key);
    CHECK(tiles__get(&tiles, key) != NULL);
    tiles__deinit(&tiles);
}

SUITE(tiles_tests,
      TEST(slot_count_follows_psram),
      TEST(loads_tile_from_sd_without_network),
      TEST(offline_missing_tile_is_remembered),
      TEST(online_download_goes_to_sd_cache),
      TEST(cached_download_is_not_fetched_again),
      TEST(http_error_leaves_no_file),
      TEST(empty_response_is_rejected),
      TEST(connects_wifi_once_and_gives_up),
      TEST(connects_wifi_when_needed),
      TEST(wrong_tile_size_is_rejected),
      TEST(evicts_least_recently_used),
      TEST(missing_list_keeps_newest),
      TEST(going_online_retries_missing))
