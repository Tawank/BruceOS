#include "bparted_common.h"

#include <errno.h> // IWYU pragma: keep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool bparted_common__parse_kind(const char *text, bruce_partition_kind_t *out_kind) {
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

void bparted_common__format_size(uint64_t bytes, char *out, size_t capacity) {
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

bool bparted_common__parse_size(const char *text, uint64_t *out_bytes) {
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

/* Only the results core/partition_manager actually produces get their own
 * wording; anything else falls through to the generic line rather than
 * claiming something specific that may not be true. */
const char *bparted_common__error_text(bruce_result_t result) {
    switch (result) {
    case BRUCE_OK: return "ok";
    case BRUCE_ERR_INVALID_ARGUMENT:
        return "bad label, type or size (labels are 1-16 of A-Z a-z 0-9 _ -, and only a swap partition may "
               "be called 'swap')";
    case BRUCE_ERR_NOT_FOUND: return "no partition with that label";
    case BRUCE_ERR_PERMISSION: return "not allowed (the root partition cannot be deleted - format it instead)";
    case BRUCE_ERR_ALREADY_EXISTS: return "a partition with that label already exists";
    case BRUCE_ERR_RESOURCE_LIMIT: return "not enough free space, or the layout is already full";
    case BRUCE_ERR_INVALID_STATE: return "that partition does not exist on flash yet";
    case BRUCE_ERR_IO: return "writing the partition table failed";
    case BRUCE_ERR_NO_MEMORY: return "out of memory";
    default: return "unexpected error";
    }
}

bruce_result_t bparted_common__clamp_list(bruce_result_t result, size_t *count, size_t capacity) {
    if (*count > capacity) *count = capacity;
    return result == BRUCE_ERR_RESOURCE_LIMIT ? BRUCE_OK : result;
}
