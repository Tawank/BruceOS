#include "partition_manager_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/partition_manager.h"

/* list_planned() can report an entry that is disappearing on top of every
 * entry that is staying, so it is the one that needs the larger buffer. */
#define SELFTEST_PARTITION_MAX (BRUCE_PARTITION_MAX_ENTRIES * 2)

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
    if (partition_manager__list_current(NULL, 0, &count) != BRUCE_OK || count == 0) {
        printf("[selftest] partition_manager/default_layout: list query failed\n");
        return false;
    }
    if (count > SELFTEST_PARTITION_MAX) count = SELFTEST_PARTITION_MAX;
    bruce_partition_entry_t entries[SELFTEST_PARTITION_MAX];
    size_t actual = 0;
    if (partition_manager__list_current(entries, count, &actual) != BRUCE_OK) {
        printf("[selftest] partition_manager/default_layout: list fetch failed\n");
        return false;
    }

    size_t root_index = 0;
    bool ok = selftest__partition_manager_find(entries, actual, BRUCE_PARTITION_ROOT_LABEL, &root_index);
    ok = ok && entries[root_index].kind == BRUCE_PARTITION_KIND_LITTLEFS && entries[root_index].size > 0 &&
         entries[root_index].is_root;
    /* Nothing is staged at this point, so the running layout is by
     * definition unchanged. */
    ok = ok && entries[root_index].state == BRUCE_PARTITION_STATE_UNCHANGED;

    bruce_partition_status_t status;
    ok = ok && partition_manager__status(&status) == BRUCE_OK;
    ok = ok && status.total_bytes > 0 && status.used_bytes > 0;

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
    ok = ok && partition_manager__stage_create(
                   BRUCE_PARTITION_SWAP_LABEL, BRUCE_PARTITION_KIND_LITTLEFS, 4096
               ) == BRUCE_ERR_INVALID_ARGUMENT;
    ok = ok && partition_manager__stage_create("myswap", BRUCE_PARTITION_KIND_SWAP, 4096) ==
                   BRUCE_ERR_INVALID_ARGUMENT;
    /* The root entry always already exists. */
    ok = ok && partition_manager__stage_create(
                   BRUCE_PARTITION_ROOT_LABEL, BRUCE_PARTITION_KIND_LITTLEFS, 4096
               ) == BRUCE_ERR_ALREADY_EXISTS;
    ok = ok && partition_manager__stage_delete(BRUCE_PARTITION_ROOT_LABEL) == BRUCE_ERR_PERMISSION;
    ok = ok && partition_manager__stage_delete("does_not_exist") == BRUCE_ERR_NOT_FOUND;
    ok = ok && partition_manager__stage_format("does_not_exist") == BRUCE_ERR_NOT_FOUND;

    if (!ok) printf("[selftest] partition_manager/validation: an invalid request was not rejected\n");
    return ok;
}

/* A staged create shows up in the planned layout tagged NEW, is absent from
 * the running one, and leaves once it is staged away again - all without a
 * commit(), so the running layout never moves. */
bool selftest__run_partition_manager_stage_lifecycle_case(void) {
    static const char *const label = "selftest_part";

    bruce_partition_status_t status;
    if (partition_manager__status(&status) != BRUCE_OK || status.max_new_size < 4096) {
        printf("[selftest] partition_manager/stage_lifecycle: not enough free space to test with\n");
        return false;
    }

    bool ok = partition_manager__stage_create(label, BRUCE_PARTITION_KIND_LITTLEFS, 4096) == BRUCE_OK;

    bruce_partition_entry_t planned[SELFTEST_PARTITION_MAX];
    size_t planned_count = 0;
    ok = ok && partition_manager__list_planned(planned, SELFTEST_PARTITION_MAX, &planned_count) == BRUCE_OK;

    size_t index = 0;
    bool found = ok && selftest__partition_manager_find(planned, planned_count, label, &index);
    ok = ok && found && planned[index].kind == BRUCE_PARTITION_KIND_LITTLEFS && planned[index].size >= 4096 &&
         planned[index].state == BRUCE_PARTITION_STATE_NEW;

    /* Staging does not move the running layout, only the planned one. */
    bruce_partition_entry_t current[SELFTEST_PARTITION_MAX];
    size_t current_count = 0;
    ok = ok && partition_manager__list_current(current, SELFTEST_PARTITION_MAX, &current_count) == BRUCE_OK;
    ok = ok && !selftest__partition_manager_find(current, current_count, label, NULL);

    /* Drop the staged entry again so the table (still only in RAM) is
     * exactly what it was before this test ran. */
    ok = (partition_manager__stage_delete(label) == BRUCE_OK) && ok;

    if (!ok) printf("[selftest] partition_manager/stage_lifecycle: staged entry did not round-trip\n");
    return ok;
}

/* status() and discard() never touch flash by themselves (only commit()
 * does) - this never calls commit() either, so it has no persisted or
 * on-flash side effects. */
bool selftest__run_partition_manager_pending_changes_case(void) {
    static const char *const label = "selftest_pend";

    bruce_partition_status_t status;
    if (partition_manager__status(&status) != BRUCE_OK || status.max_new_size < 4096) {
        printf("[selftest] partition_manager/pending_changes: not enough free space to test with\n");
        return false;
    }

    size_t planned_before = 0;
    bool ok = partition_manager__list_planned(NULL, 0, &planned_before) == BRUCE_OK;
    ok = ok && !status.has_pending_changes;

    ok = ok && partition_manager__stage_create(label, BRUCE_PARTITION_KIND_LITTLEFS, 4096) == BRUCE_OK;
    ok = ok && partition_manager__status(&status) == BRUCE_OK && status.has_pending_changes;

    size_t planned_after_stage = 0;
    ok = ok && partition_manager__list_planned(NULL, 0, &planned_after_stage) == BRUCE_OK;
    ok = ok && planned_after_stage == planned_before + 1;

    /* Discarding drops the staged entry and clears the pending flag, without
     * ever having called commit(). */
    ok = ok && partition_manager__discard() == BRUCE_OK;
    ok = ok && partition_manager__status(&status) == BRUCE_OK && !status.has_pending_changes;

    size_t planned_after_discard = 0;
    ok = ok && partition_manager__list_planned(NULL, 0, &planned_after_discard) == BRUCE_OK;
    ok = ok && planned_after_discard == planned_before;

    if (!ok) printf("[selftest] partition_manager/pending_changes: staged/planned views did not agree\n");
    return ok;
}
