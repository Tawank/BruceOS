#include "partition_manager_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *partition_manager_common__kind_name(bruce_partition_kind_t kind) {
    return kind == BRUCE_PARTITION_KIND_SWAP ? "swap" : "littlefs";
}

bool partition_manager_common__parse_kind(const char *text, bruce_partition_kind_t *out_kind) {
    if (text == NULL || out_kind == NULL) return false;
    if (strcmp(text, "swap") == 0) {
        *out_kind = BRUCE_PARTITION_KIND_SWAP;
        return true;
    }
    if (strcmp(text, "littlefs") == 0) {
        *out_kind = BRUCE_PARTITION_KIND_LITTLEFS;
        return true;
    }
    return false;
}

void partition_manager_common__format_size(uint64_t bytes, char *out, size_t capacity) {
    static const char units[] = {'B', 'K', 'M', 'G', 'T'};
    uint64_t divisor = 1;
    size_t unit = 0;
    while (unit + 1 < sizeof(units) && bytes >= divisor * 1024) {
        divisor *= 1024;
        unit++;
    }
    uint64_t whole = bytes / divisor;
    uint64_t tenth = ((bytes % divisor) * 10) / divisor;
    if (unit == 0 || tenth == 0) {
        snprintf(out, capacity, "%llu%c", (unsigned long long)whole, units[unit]);
    } else {
        snprintf(
            out, capacity, "%llu.%llu%c", (unsigned long long)whole, (unsigned long long)tenth, units[unit]
        );
    }
}

bool partition_manager_common__parse_size(const char *text, uint64_t *out_bytes) {
    if (text == NULL || text[0] == '\0' || out_bytes == NULL) return false;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text) return false;

    uint64_t multiplier = 1;
    if (*end == 'k' || *end == 'K') {
        multiplier = 1024ull;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 1024ull * 1024ull;
        end++;
    } else if (*end == 'g' || *end == 'G') {
        multiplier = 1024ull * 1024ull * 1024ull;
        end++;
    }
    if (*end != '\0' || value == 0 || multiplier > UINT64_MAX / value) return false;
    *out_bytes = value * multiplier;
    return true;
}
