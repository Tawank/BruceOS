#pragma once

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern int test_failures;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            test_failures++;                                                                \
        }                                                                                   \
    } while (0)

#define CHECK_EQ(actual, expected) CHECK((actual) == (expected))
#define CHECK_STR(actual, expected) CHECK(strcmp((actual), (expected)) == 0)
#define CHECK_NEAR(actual, expected, tolerance) CHECK(fabs((double)(actual) - (double)(expected)) <= (tolerance))

typedef struct {
    const char *name;
    void (*run)(void);
} test_case_t;

#define TEST(fn) {#fn, fn}
#define SUITE(name, ...)                         \
    const test_case_t name[] = {__VA_ARGS__};    \
    const size_t name##_count = sizeof(name) / sizeof(name[0]);
