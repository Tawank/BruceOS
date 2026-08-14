/* ESP-IDF platform hooks needed by statically linked WAMR external ELF apps. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "core_sdk/runtime.h"

void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

int64_t esp_timer_get_time(void) { return (int64_t)(runtime__now() * 1000u); }
