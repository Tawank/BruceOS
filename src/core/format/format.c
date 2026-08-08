#include "core_sdk/format.h"

#include <stdio.h>

void format__bytes_human(uint64_t bytes, char *output, size_t capacity) {
    if (output == NULL || capacity == 0) return;
    static const char units[] = {'B', 'K', 'M', 'G', 'T'};
    uint64_t divisor = 1;
    size_t unit = 0;
    while (unit + 1 < sizeof(units) && bytes >= divisor * 1024) {
        divisor *= 1024;
        unit++;
    }
    uint64_t whole = bytes / divisor;
    uint64_t tenth = ((bytes % divisor) * 10) / divisor;
    if (unit == 0 || bytes < 10000) {
        /* Plain byte counts get no unit suffix -- just the number. */
        snprintf(output, capacity, "%llu", (unsigned long long)bytes);
    } else if (tenth == 0) {
        snprintf(output, capacity, "%llu%c", (unsigned long long)whole, units[unit]);
    } else {
        snprintf(
            output, capacity, "%llu.%llu%c", (unsigned long long)whole, (unsigned long long)tenth, units[unit]
        );
    }
}
