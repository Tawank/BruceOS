#include <stdbool.h>
#include <stddef.h>

#include "fakes.h"
#include "test.h"

int test_failures;

#define DECLARE_SUITE(name)            \
    extern const test_case_t name[]; \
    extern const size_t name##_count;
DECLARE_SUITE(geo_tests)
DECLARE_SUITE(map_view_tests)
DECLARE_SUITE(tiles_tests)
DECLARE_SUITE(settings_tests)
DECLARE_SUITE(hud_tests)
DECLARE_SUITE(maps_tests)

static int run_suite(const char *suite, const test_case_t *tests, size_t count) {
    int failed = 0;
    for (size_t i = 0; i < count; ++i) {
        int before = test_failures;
        fakes__reset();
        tests[i].run();
        bool ok = test_failures == before;
        printf("%s %s/%s\n", ok ? "ok  " : "FAIL", suite, tests[i].name);
        failed += !ok;
    }
    return failed;
}

int main(void) {
    int failed = 0;
    failed += run_suite("geo", geo_tests, geo_tests_count);
    failed += run_suite("map_view", map_view_tests, map_view_tests_count);
    failed += run_suite("tiles", tiles_tests, tiles_tests_count);
    failed += run_suite("settings", settings_tests, settings_tests_count);
    failed += run_suite("hud", hud_tests, hud_tests_count);
    failed += run_suite("maps", maps_tests, maps_tests_count);
    printf("\n%s: %d failed test(s)\n", failed ? "FAILED" : "PASSED", failed);
    return failed ? 1 : 0;
}
