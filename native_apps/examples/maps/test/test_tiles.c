#include "fakes.h"
#include "test.h"
#include "tiles.h"

#define DIR "/sdcard/maps/tiles"
#define URL "https://tiles.test/{z}/{x}/{y}.png?key=abc"
#define TILE_BYTES (256 * 256 * 2)

static tiles_t tiles;

static void init(bool online) { CHECK(tiles__init(&tiles, 6, DIR, URL, online)); }

/* Download, store on SD, then decode: the whole trip for one tile. */
static void fetch(tile_key_t key) {
    CHECK_EQ(tiles__load(&tiles, key), TILE_DOWNLOADING);
    CHECK(tiles__poll(&tiles));
    CHECK_EQ(tiles__load(&tiles, key), TILE_LOADED);
}

static bool keep_x_one(tile_key_t key, void *context) {
    (void)context;
    return key.x == 1;
}

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
    CHECK(tiles__needs_load(&tiles, key));
    CHECK_EQ(tiles__load(&tiles, key), TILE_LOADED);
    CHECK(tiles__get(&tiles, key) != NULL);
    CHECK(!tiles__needs_load(&tiles, key));
    CHECK_EQ(fakes.http_calls, 0);
    tiles__deinit(&tiles);
}

static void offline_missing_tile_is_remembered(void) {
    init(false);
    tile_key_t key = {3, 4, 2};
    CHECK_EQ(tiles__load(&tiles, key), TILE_MISSING);
    CHECK(tiles__is_missing(&tiles, key));
    CHECK(!tiles__needs_load(&tiles, key));
    CHECK_EQ(fakes.http_calls, 0);
    CHECK_EQ(fakes.wifi_connect_calls, 0);
    tiles__deinit(&tiles);
}

static void download_lands_in_sd_cache(void) {
    init(true);
    tile_key_t key = {12, 2271, 1388};
    CHECK_EQ(tiles__load(&tiles, key), TILE_DOWNLOADING);
    CHECK_STR(fakes.http_url, "https://tiles.test/12/2271/1388.png?key=abc");
    CHECK_STR(fakes.http_user_agent, "BruceOS-Maps/0.1");
    CHECK(!tiles__needs_load(&tiles, key));

    CHECK(tiles__poll(&tiles));
    CHECK(fakes__file_exists(DIR "/12/2271/1388.png"));
    CHECK(!fakes__file_exists(DIR "/12/2271/1388.png.part"));
    CHECK(fakes.mkdir_count >= 5);
    CHECK(tiles__needs_load(&tiles, key));
    CHECK_EQ(tiles__load(&tiles, key), TILE_LOADED);
    CHECK_EQ(fakes.http_calls, 1);
    tiles__deinit(&tiles);
}

static void download_runs_in_background(void) {
    fakes.http_manual = true;
    init(true);
    tile_key_t key = {5, 1, 1};
    CHECK_EQ(tiles__load(&tiles, key), TILE_DOWNLOADING);
    CHECK(!tiles__poll(&tiles));
    CHECK(!tiles__poll(&tiles));
    CHECK(!fakes__file_exists(DIR "/5/1/1.png"));
    fakes__http_finish_all();
    CHECK(tiles__poll(&tiles));
    CHECK(fakes__file_exists(DIR "/5/1/1.png"));
    tiles__deinit(&tiles);
}

static void at_most_five_downloads_at_once(void) {
    fakes.http_manual = true;
    init(true);
    for (int x = 0; x < TILES_DOWNLOADS_MAX; ++x) CHECK_EQ(tiles__load(&tiles, (tile_key_t){9, x, 0}), TILE_DOWNLOADING);
    tile_key_t extra = {9, 7, 0};
    CHECK_EQ(tiles__load(&tiles, extra), TILE_DEFERRED);
    CHECK(tiles__needs_load(&tiles, extra));
    CHECK_EQ(fakes.http_calls, TILES_DOWNLOADS_MAX);
    CHECK_EQ(fakes.http_max_active, TILES_DOWNLOADS_MAX);

    fakes__http_finish_all();
    CHECK(tiles__poll(&tiles));
    CHECK_EQ(tiles.download_count, 0u);
    CHECK_EQ(tiles__load(&tiles, extra), TILE_DOWNLOADING);
    tiles__deinit(&tiles);
}

static void core_job_limit_defers(void) {
    fakes.http_start_result = BRUCE_ERR_RESOURCE_LIMIT;
    init(true);
    tile_key_t key = {5, 1, 1};
    CHECK_EQ(tiles__load(&tiles, key), TILE_DEFERRED);
    CHECK(!tiles__is_missing(&tiles, key));
    tiles__deinit(&tiles);
}

static void failed_downloads_leave_no_file(void) {
    fakes.http_status = 404;
    init(true);
    tile_key_t key = {5, 1, 1};
    tiles__load(&tiles, key);
    tiles__poll(&tiles);
    CHECK(!fakes__file_exists(DIR "/5/1/1.png"));
    CHECK(!fakes__file_exists(DIR "/5/1/1.png.part"));
    CHECK(tiles__is_missing(&tiles, key));

    fakes.http_status = 200;
    fakes.http_result = BRUCE_ERR_IO;
    tile_key_t other = {5, 2, 1};
    tiles__load(&tiles, other);
    tiles__poll(&tiles);
    CHECK(tiles__is_missing(&tiles, other));
    CHECK(!tiles.network_down);
    tiles__deinit(&tiles);
}

static void lost_wifi_during_download_shows_no_wifi(void) {
    fakes.http_manual = true;
    init(true);
    tiles__load(&tiles, (tile_key_t){5, 1, 1});
    fakes.wifi_connected = false;
    fakes.http_result = BRUCE_ERR_IO;
    fakes__http_finish_all();
    tiles__poll(&tiles);
    CHECK(tiles.network_down);
    tiles__deinit(&tiles);
}

static void empty_response_is_rejected(void) {
    fakes.http_body_len = 0;
    init(true);
    tiles__load(&tiles, (tile_key_t){5, 1, 1});
    tiles__poll(&tiles);
    CHECK(!fakes__file_exists(DIR "/5/1/1.png"));
    tiles__deinit(&tiles);
}

static void cancel_unless_drops_unwanted_downloads(void) {
    fakes.http_manual = true;
    init(true);
    for (int x = 0; x < 3; ++x) tiles__load(&tiles, (tile_key_t){9, x, 0});
    CHECK(tiles__cancel_unless(&tiles, keep_x_one, NULL));
    CHECK(!tiles__cancel_unless(&tiles, keep_x_one, NULL));
    CHECK_EQ(fakes.http_cancels, 2);
    CHECK_EQ(tiles.download_count, 1u);
    CHECK_EQ(tiles.downloads[0].key.x, 1);
    CHECK(tiles__needs_load(&tiles, (tile_key_t){9, 0, 0}));
    CHECK(!tiles__is_missing(&tiles, (tile_key_t){9, 2, 0}));
    tiles__deinit(&tiles);
}

static void going_offline_and_deinit_cancel_downloads(void) {
    fakes.http_manual = true;
    init(true);
    tiles__load(&tiles, (tile_key_t){9, 0, 0});
    tiles__load(&tiles, (tile_key_t){9, 1, 0});
    tiles__set_online(&tiles, false);
    CHECK_EQ(fakes.http_cancels, 2);
    CHECK_EQ(tiles.download_count, 0u);

    tiles__set_online(&tiles, true);
    tiles__load(&tiles, (tile_key_t){9, 2, 0});
    tiles__deinit(&tiles);
    CHECK_EQ(fakes.http_cancels, 3);
    CHECK_EQ(fakes__http_active(), 0);
}

/* Not connected and no saved network in range. */
static void offline_start(void) {
    fakes.wifi_connected = false;
    fakes.wifi_connect_ok = false;
}

static void connects_wifi_in_background(void) {
    offline_start();
    fakes.wifi_connect_ok = true;
    fakes.wifi_connect_manual = true;
    init(true);
    tile_key_t key = {5, 1, 1};
    CHECK_EQ(tiles__load(&tiles, key), TILE_MISSING);
    CHECK(tiles.connecting);
    CHECK_EQ(fakes.wifi_connect_calls, 1);
    CHECK(!tiles__check_network(&tiles));
    CHECK_EQ(tiles__load(&tiles, (tile_key_t){5, 2, 1}), TILE_MISSING);
    CHECK_EQ(fakes.wifi_connect_calls, 1);

    fakes__wifi_finish();
    CHECK(tiles__check_network(&tiles));
    CHECK(!tiles.connecting);
    CHECK(!tiles.network_down);
    CHECK(!tiles__is_missing(&tiles, key));
    fetch(key);
    tiles__deinit(&tiles);
}

static void failed_connect_shows_no_wifi(void) {
    offline_start();
    init(true);
    CHECK_EQ(tiles__load(&tiles, (tile_key_t){5, 1, 1}), TILE_MISSING);
    CHECK(tiles__check_network(&tiles));
    CHECK(tiles.network_down);
    CHECK(!tiles.connecting);
    CHECK_EQ(tiles__load(&tiles, (tile_key_t){5, 2, 1}), TILE_MISSING);
    CHECK_EQ(fakes.wifi_connect_calls, 1);
    CHECK_EQ(fakes.http_calls, 0);
    tiles__deinit(&tiles);
}

static void wrong_tile_size_is_rejected(void) {
    fakes.decode_size = 128;
    fakes__add_file(DIR "/3/4/2.png", 500);
    init(false);
    tile_key_t key = {3, 4, 2};
    CHECK_EQ(tiles__load(&tiles, key), TILE_MISSING);
    CHECK(tiles__get(&tiles, key) == NULL);
    tiles__deinit(&tiles);
}

static void evicts_least_recently_used(void) {
    fakes.psram_free = 2 * TILE_BYTES;
    fakes__add_file(DIR "/5/1/1.png", 1);
    fakes__add_file(DIR "/5/2/1.png", 1);
    fakes__add_file(DIR "/5/3/1.png", 1);
    CHECK(tiles__init(&tiles, 2, DIR, URL, false));
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
    offline_start();
    init(true);
    tile_key_t key = {5, 1, 1};
    tiles__load(&tiles, key);
    tiles__check_network(&tiles);
    CHECK(tiles__is_missing(&tiles, key));

    tiles__set_online(&tiles, true);
    CHECK(!tiles.network_down);
    CHECK(!tiles__is_missing(&tiles, key));
    fakes.wifi_connect_ok = true;
    CHECK_EQ(tiles__load(&tiles, key), TILE_MISSING);
    CHECK_EQ(fakes.wifi_connect_calls, 2);
    CHECK(tiles__check_network(&tiles));
    fetch(key);
    tiles__deinit(&tiles);
}

static void wifi_turned_on_elsewhere_clears_no_wifi(void) {
    offline_start();
    init(true);
    tile_key_t key = {5, 1, 1};
    tiles__load(&tiles, key);
    CHECK(tiles__check_network(&tiles));
    CHECK(tiles.network_down);

    fakes.wifi_connected = true;
    fakes.now_ms = 500;
    CHECK(!tiles__check_network(&tiles));
    fakes.now_ms = 1000;
    CHECK(tiles__check_network(&tiles));
    CHECK(!tiles.network_down);
    CHECK(!tiles__is_missing(&tiles, key));
    CHECK(!tiles__check_network(&tiles));
    fetch(key);
    tiles__deinit(&tiles);
}

static void retries_saved_wifi_every_30_seconds(void) {
    offline_start();
    init(true);
    tiles__load(&tiles, (tile_key_t){5, 1, 1});
    tiles__check_network(&tiles);
    fakes.now_ms = 29999;
    tiles__load(&tiles, (tile_key_t){5, 2, 1});
    CHECK_EQ(fakes.wifi_connect_calls, 1);

    fakes.now_ms = 30000;
    fakes.wifi_connect_ok = true;
    tiles__load(&tiles, (tile_key_t){5, 3, 1});
    CHECK_EQ(fakes.wifi_connect_calls, 2);
    CHECK(tiles__check_network(&tiles));
    CHECK(!tiles.network_down);
    CHECK(!tiles__is_missing(&tiles, (tile_key_t){5, 1, 1}));
    fetch((tile_key_t){5, 3, 1});
    tiles__deinit(&tiles);
}

static void going_offline_stops_following_connect(void) {
    offline_start();
    fakes.wifi_connect_manual = true;
    init(true);
    tiles__load(&tiles, (tile_key_t){5, 1, 1});
    CHECK(tiles.connecting);
    tiles__set_online(&tiles, false);
    CHECK(!tiles.connecting);
    CHECK(!tiles__check_network(&tiles));
    CHECK_EQ(tiles__load(&tiles, (tile_key_t){5, 2, 1}), TILE_MISSING);
    CHECK_EQ(fakes.wifi_connect_calls, 1);
    tiles__deinit(&tiles);
}

SUITE(tiles_tests,
      TEST(slot_count_follows_psram),
      TEST(loads_tile_from_sd_without_network),
      TEST(offline_missing_tile_is_remembered),
      TEST(download_lands_in_sd_cache),
      TEST(download_runs_in_background),
      TEST(at_most_five_downloads_at_once),
      TEST(core_job_limit_defers),
      TEST(failed_downloads_leave_no_file),
      TEST(lost_wifi_during_download_shows_no_wifi),
      TEST(empty_response_is_rejected),
      TEST(cancel_unless_drops_unwanted_downloads),
      TEST(going_offline_and_deinit_cancel_downloads),
      TEST(connects_wifi_in_background),
      TEST(failed_connect_shows_no_wifi),
      TEST(wrong_tile_size_is_rejected),
      TEST(evicts_least_recently_used),
      TEST(missing_list_keeps_newest),
      TEST(going_online_retries_missing),
      TEST(wifi_turned_on_elsewhere_clears_no_wifi),
      TEST(retries_saved_wifi_every_30_seconds),
      TEST(going_offline_stops_following_connect))
