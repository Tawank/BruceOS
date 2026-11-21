#include <stdbool.h>
#include <stddef.h>

#include "fakes.h"
#include "test.h"

int test_failures;

#define DECLARE_SUITE(name)                                                                                  \
    extern const test_case_t name[];                                                                         \
    extern const size_t name##_count;
DECLARE_SUITE(book_tests)
DECLARE_SUITE(ui_tests)
DECLARE_SUITE(phone_tests)

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
    failed += run_suite("book", book_tests, book_tests_count);
    failed += run_suite("ui", ui_tests, ui_tests_count);
    failed += run_suite("phone", phone_tests, phone_tests_count);
    printf("%d failed\n", failed);
    return failed != 0;
}
