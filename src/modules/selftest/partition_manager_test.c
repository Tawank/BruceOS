#include "partition_manager_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/partition_manager.h"

static bool selftest__partition_manager_find(
    const bruce_partition_entry_t *entries, size_t count, const char *label, size_t *out_index
) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].label, label) == 0) {
            if (out_index != NULL) *out_index = i;
            return true;
        }
    }
    return false;
}

/* Every device - legacy (no committed table) or not - always has exactly one
 * "littlefs" entry, the one storage__init() mounts at "/" (see
 * core/partition_manager/partition_manager.c's ensure_init_locked()). */
bool selftest__run_partition_manager_default_layout_case(void) {
    size_t count = 0;
    if (partition_manager__list(NULL, 0, &count) != BRUCE_OK || count == 0) {
        printf("[selftest] partition_manager/default_layout: list query failed\n");
        return false;
    }
    if (count > 16) count = 16;
    bruce_partition_entry_t entries[16];
    size_t actual = 0;
    if (partition_manager__list(entries, count, &actual) != BRUCE_OK) {
        printf("[selftest] partition_manager/default_layout: list fetch failed\n");
        return false;
    }

    size_t root_index = 0;
    bool has_root = selftest__partition_manager_find(entries, actual, "littlefs", &root_index);
    bool ok =
        has_root && entries[root_index].kind == BRUCE_PARTITION_KIND_LITTLEFS && entries[root_index].size > 0;

    uint64_t free_bytes = 0;
    ok = ok && partition_manager__free_space(&free_bytes) == BRUCE_OK;
    if (!ok) printf("[selftest] partition_manager/default_layout: no valid root littlefs entry\n");
    return ok;
}

/* Staging (stage_create/stage_delete/stage_format) only mutates in-RAM
 * state; only commit() ever touches flash. These checks intentionally never
 * call commit(), so they have no persisted or on-flash side effects. */
bool selftest__run_partition_manager_validation_case(void) {
    bool ok = true;

    ok = ok &&
         partition_manager__stage_create("", BRUCE_PARTITION_KIND_LITTLEFS, 4096) == BRUCE_ERR_INVALID_ARGUMENT;
    ok = ok && partition_manager__stage_create("bad label", BRUCE_PARTITION_KIND_LITTLEFS, 4096) ==
                   BRUCE_ERR_INVALID_ARGUMENT;
    /* "swap" is reserved for BRUCE_PARTITION_KIND_SWAP, and vice versa. */
    ok = ok && partition_manager__stage_create("swap", BRUCE_PARTITION_KIND_LITTLEFS, 4096) ==
                   BRUCE_ERR_INVALID_ARGUMENT;
    ok = ok && partition_manager__stage_create("myswap", BRUCE_PARTITION_KIND_SWAP, 4096) ==
                   BRUCE_ERR_INVALID_ARGUMENT;
    /* The root "littlefs" entry always already exists. */
    ok = ok && partition_manager__stage_create("littlefs", BRUCE_PARTITION_KIND_LITTLEFS, 4096) ==
                   BRUCE_ERR_ALREADY_EXISTS;
    ok = ok && partition_manager__stage_delete("littlefs") == BRUCE_ERR_PERMISSION;
    ok = ok && partition_manager__stage_delete("does_not_exist") == BRUCE_ERR_NOT_FOUND;
    ok = ok && partition_manager__stage_format("does_not_exist") == BRUCE_ERR_NOT_FOUND;

    if (!ok) printf("[selftest] partition_manager/validation: an invalid request was not rejected\n");
    return ok;
}

bool selftest__run_partition_manager_stage_lifecycle_case(void) {
    static const char *const label = "selftest_part";

    uint64_t free_before = 0;
    if (partition_manager__free_space(&free_before) != BRUCE_OK || free_before < 8192) {
        printf("[selftest] partition_manager/stage_lifecycle: not enough free space to test with\n");
        return false;
    }

    bool ok = partition_manager__stage_create(label, BRUCE_PARTITION_KIND_LITTLEFS, 4096) == BRUCE_OK;
    ok = ok && partition_manager__reboot_required();

    size_t count = 0;
    ok = ok && partition_manager__list(NULL, 0, &count) == BRUCE_OK;
    if (ok && count > 16) count = 16;
    bruce_partition_entry_t entries[16];
    size_t actual = 0;
    ok = ok && partition_manager__list(entries, count, &actual) == BRUCE_OK;

    size_t index = 0;
    bool found = ok && selftest__partition_manager_find(entries, actual, label, &index);
    ok = ok && found && entries[index].kind == BRUCE_PARTITION_KIND_LITTLEFS &&
         entries[index].size >= 4096 && entries[index].format_pending;

    /* Drop the staged entry again so the table (still only in RAM) is
     * exactly what it was before this test ran. */
    ok = partition_manager__stage_delete(label) == BRUCE_OK && ok;

    if (!ok) printf("[selftest] partition_manager/stage_lifecycle: staged entry did not round-trip\n");
    return ok;
}
