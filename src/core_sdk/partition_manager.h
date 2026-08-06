#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/disk.h" // IWYU pragma: export (BRUCE_DISK_NAME_MAX)
#include "core_sdk/result.h"

/*
 * Manages the "user area": the flash left over after the static partitions
 * in partitions.csv (bootloader, partition_table, nvs, coredump, factory),
 * minus one sector at the very end of the flash reserved for this module's
 * own partition table. The user area always holds exactly one LittleFS
 * partition labeled "littlefs" mounted at "/" (BRUCE_PARTITION_ROOT_LABEL),
 * at offset 0, plus any number of extra partitions the user creates.
 *
 * The root partition is elastic: it is never given a size directly, it
 * simply spans everything below the lowest extra partition. Creating an
 * extra partition therefore takes that space away from "/" and reformats it
 * on the next boot (LittleFS cannot be shrunk in place); deleting the lowest
 * extra partition gives the space back the same way. Both UIs
 * (modules/bparted) warn about that before anything is written.
 *
 * Nothing here ever mounts, erases or reformats storage the running system
 * is using: stage_create()/stage_delete()/stage_format() only edit an
 * in-RAM working table, commit() persists that table, and the erase/format
 * work it describes happens once, automatically, early on the next boot -
 * see list_current() vs list_planned() and bruce_partition_status_t's
 * reboot_required.
 *
 * "swap" (BRUCE_PARTITION_SWAP_LABEL) is a reserved label: a
 * BRUCE_PARTITION_KIND_SWAP entry is always labeled "swap" and uses the
 * exact type/subtype core/memory's flash-backed swap allocator already
 * looks for, so creating one is all that is needed to enable it. Any other
 * label is an additional LittleFS volume that gets formatted but is not
 * auto-mounted.
 *
 * Mutating calls (stage_create/stage_delete/stage_format/commit/discard)
 * are restricted to built-in modules (see the "bparted" command), the same
 * restriction core_sdk/disk.h's disk__mount()/disk__unmount() use; any app
 * may read the layout.
 */

#define BRUCE_PARTITION_LABEL_MAX BRUCE_DISK_NAME_MAX

/* Upper bound on the entries one layout can hold, root included. Callers
 * that want the whole table in one call can size a stack array with it. */
#define BRUCE_PARTITION_MAX_ENTRIES 8

/* The LittleFS partition mounted at "/", present in every layout. It can be
 * reformatted, never deleted. */
#define BRUCE_PARTITION_ROOT_LABEL "littlefs"

/* The flash-backed swap partition core/memory looks for. */
#define BRUCE_PARTITION_SWAP_LABEL "swap"

/* Space the root partition always keeps, however much the user carves out
 * of it (config, launcher menu, ... all live there). */
#define BRUCE_PARTITION_ROOT_MIN_BYTES (64u * 1024u)

typedef enum {
    BRUCE_PARTITION_KIND_SWAP,
    BRUCE_PARTITION_KIND_LITTLEFS,
} bruce_partition_kind_t;

/* How a planned entry differs from the layout running right now. Every
 * entry list_current() returns is BRUCE_PARTITION_STATE_UNCHANGED. */
typedef enum {
    BRUCE_PARTITION_STATE_UNCHANGED,
    BRUCE_PARTITION_STATE_NEW,     /* Does not exist yet; created on the next boot. */
    BRUCE_PARTITION_STATE_DELETED, /* Exists now; gone after the next boot. */
    BRUCE_PARTITION_STATE_FORMAT,  /* Exists now; erased/reformatted on the next boot. */
} bruce_partition_state_t;

typedef struct {
    char label[BRUCE_PARTITION_LABEL_MAX];
    bruce_partition_kind_t kind;
    uint64_t offset; /* Relative to the start of the user area. */
    uint64_t size;
    bruce_partition_state_t state;
    bool is_root; /* The BRUCE_PARTITION_ROOT_LABEL entry mounted at "/". */
} bruce_partition_entry_t;

typedef struct {
    /* An edit is staged that commit() has not written yet - i.e. list_planned()
     * shows something discard() would take back. */
    bool has_pending_changes;
    /* A committed layout differs from the one running now, so a reboot is
     * needed to apply it. Independent of has_pending_changes. */
    bool reboot_required;
    uint64_t total_bytes;       /* Size of the whole user area. */
    uint64_t used_bytes;        /* Covered by planned entries, root included. */
    uint64_t unallocated_bytes; /* Gaps between extra partitions, usable only by a new one. */
    uint64_t max_new_size;      /* Largest partition stage_create() would accept right now. */
} bruce_partition_status_t;

/* The layout running this boot. Pass NULL with capacity 0 to query count;
 * a too-small non-NULL buffer fills what fits and returns
 * BRUCE_ERR_RESOURCE_LIMIT. Entries are ordered by offset. */
bruce_result_t
partition_manager__list_current(bruce_partition_entry_t *entries, size_t capacity, size_t *out_count);

/* The layout the next boot will have: list_current() plus every staged and
 * committed change, each entry tagged with how it differs (including
 * BRUCE_PARTITION_STATE_DELETED rows for entries that are about to go away,
 * which is why this can return more entries than list_current()). Same
 * ordering and capacity convention as list_current(). */
bruce_result_t
partition_manager__list_planned(bruce_partition_entry_t *entries, size_t capacity, size_t *out_count);

/* Space accounting plus the two "is there anything outstanding" flags. */
bruce_result_t partition_manager__status(bruce_partition_status_t *out_status);

/* Stages a new partition, taking its space from the tail of the root
 * partition (or from a gap a previous delete left behind). `label` must be
 * 1-16 chars of [A-Za-z0-9_-] and a BRUCE_PARTITION_KIND_SWAP entry's label
 * must be exactly BRUCE_PARTITION_SWAP_LABEL. `size_bytes` is rounded up to
 * the nearest 4096-byte flash sector. Returns BRUCE_ERR_ALREADY_EXISTS for
 * a duplicate label and BRUCE_ERR_RESOURCE_LIMIT when the table is full or
 * the request is larger than bruce_partition_status_t.max_new_size. Call
 * commit() to persist. */
bruce_result_t
partition_manager__stage_create(const char *label, bruce_partition_kind_t kind, uint64_t size_bytes);

/* Stages removal of `label`. The root entry cannot be deleted
 * (BRUCE_ERR_PERMISSION) - reformat it instead. Its space is given back to
 * the root partition when it sits directly above it, and is otherwise left
 * as a gap only a new partition can reuse. */
bruce_result_t partition_manager__stage_delete(const char *label);

/* Stages an in-place erase + reformat of `label`, keeping its size, kind
 * and position. Allowed for the root entry (wipes and reformats "/" on the
 * next boot). Returns BRUCE_ERR_INVALID_STATE for an entry that does not
 * exist yet (a staged create is formatted on creation anyway). */
bruce_result_t partition_manager__stage_format(const char *label);

/* Persists the staged table to flash. Does not itself erase or format
 * anything - that happens once, automatically, the next time the device
 * boots. */
bruce_result_t partition_manager__commit(void);

/* Discards every not-yet-committed staged edit, resetting list_planned()
 * back to the last committed layout. A no-op (BRUCE_OK) when
 * has_pending_changes is already false. */
bruce_result_t partition_manager__discard(void);
