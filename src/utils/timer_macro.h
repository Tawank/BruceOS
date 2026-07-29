#include "esp_timer.h" // IWYU pragma: keep

#define TIMER_INIT() uint64_t start = esp_timer_get_time()
#define TIMER_PRINT(msg)                                                                                     \
    printf(msg " in %lld ms\n", (esp_timer_get_time() - start) / 1000);                                      \
    start = esp_timer_get_time()
