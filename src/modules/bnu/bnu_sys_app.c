#include "bnu_app.h"
#include "bnu_internal.h"

#include <errno.h> // IWYU pragma: keep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/clock.h"
#include "core_sdk/device.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"

/* System commands: free, top, shutdown, reboot, stty, date, sleep. */

/* Heap-allocated, not a local array: on top of the legend/block-map buffers
 * `free -m` already keeps on the heap, this is another ~1.7K that a stack
 * array would add on top of the caller's own frame, and this runs on that
 * process's own (small, default-sized) stack. */
#define BNU_PROCESS_SNAPSHOT_MAX 16

#define BNU_MEMORY_LAYOUT_WIDTH 48

static const bruce_process_snapshot_t *
bnu__layout_process(const bruce_process_snapshot_t *processes, size_t count, bruce_process_id_t id) {
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].id == id) return &processes[i];
    }
    return NULL;
}

/* One row per (owner, used-or-reserved, is-stack) combination that actually
 * appears in a backend's blocks - built fresh per `free -m` call from
 * whichever processes and reservations are live right now, rather than
 * reserving a fixed letter per system process up front. That's what lets the
 * legend list only processes actually visible in the map (see
 * bnu__build_layout_legend()) instead of every process on the system. Each
 * entry also totals the bytes it stands for and is always printed regardless
 * of whether it ends up visible on the map - a block too small to earn its
 * own map cell (see bnu__print_layout_region()'s rounding) is never the only
 * way to find out who's actually there and how much they hold.
 *
 * On RAM backends (internal/PSRAM) "reserved" bytes - the rounded-up tail an
 * allocator pads a block with beyond what was actually requested - are never
 * split out per owner here: unlike a swap slab page's spare slots (which
 * stay reserved for that exact owner and nobody else), RAM padding isn't
 * usable by any process either way, so whose padding it happens to be
 * doesn't matter. bnu__build_layout_legend() folds it all into one combined
 * total instead (its compact_bytes out-parameter), painted with a single
 * fixed '+' rather than a per-owner letter. */
#define BNU_MEMORY_LEGEND_MAX 52

typedef struct {
    bruce_process_id_t owner_id;
    /* false: this entry's bytes are actually holding that owner's data.
     * true: swap bytes reserved for that owner but not currently holding any
     * - a free slot run on a slab page it exclusively holds (see
     * memory_external__allocate_slot_locked()'s doc comment - such a page's
     * spare slots aren't available to anyone else, so "free" alone would be
     * misleading). RAM backends never set this - see BNU_MEMORY_LEGEND_MAX's
     * doc comment above. */
    bool reserved;
    /* True when these bytes are the process's own task-stack buffer rather
     * than a regular heap allocation - see bruce_memory_layout_block_t's
     * is_stack field. Keeping it part of the grouping key (rather than
     * folding it into the owner's regular "used" total) is what lets the
     * map/legend call it out as e.g. "browser (stack)" instead of silently
     * padding out that process's ordinary heap usage. */
    bool is_stack;
    char symbol;
    size_t bytes;
} bnu__layout_legend_entry_t;

/* Index lookup: turning an (owner, reserved, is_stack) key back into
 * legend[]'s index, e.g. so a block can be credited to the same row
 * bnu__build_layout_legend() already gave it. SIZE_MAX on the near-impossible
 * "ran out of the 52 available letters" overflow (callers fall back to a
 * dedicated overflow category - see BNU_MEMORY_CATEGORY_OVERFLOW). */
static size_t bnu__layout_legend_index(
    const bnu__layout_legend_entry_t *legend, size_t legend_count, bruce_process_id_t owner_id, bool reserved,
    bool is_stack
) {
    for (size_t i = 0; i < legend_count; ++i) {
        if (legend[i].owner_id == owner_id && legend[i].reserved == reserved && legend[i].is_stack == is_stack) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void bnu__layout_legend_add_bytes(
    bnu__layout_legend_entry_t *legend, size_t *legend_count, bruce_process_id_t owner_id, bool reserved,
    bool is_stack, size_t bytes
) {
    for (size_t i = 0; i < *legend_count; ++i) {
        if (legend[i].owner_id == owner_id && legend[i].reserved == reserved && legend[i].is_stack == is_stack) {
            legend[i].bytes += bytes;
            return;
        }
    }
    if (*legend_count >= BNU_MEMORY_LEGEND_MAX) return;
    size_t index = (*legend_count)++;
    legend[index] = (bnu__layout_legend_entry_t){
        .symbol = index < 26 ? (char)('A' + index) : (char)('a' + (index - 26)),
        .owner_id = owner_id,
        .reserved = reserved,
        .is_stack = is_stack,
        .bytes = bytes,
    };
}

/* Scans every block this backend reported (not just one region) so the same
 * owner gets the same letter in every region's map, and assigns letters in
 * address order (blocks are already sorted by the time this runs) - the
 * closest thing to "in the order you'd read the maps" without tracking each
 * region separately.
 *
 * compact_reserved folds every "reserved" byte (regardless of owner) into
 * *out_padding_bytes instead of a per-owner legend entry - see
 * BNU_MEMORY_LEGEND_MAX's doc comment. Pass false, as the swap backend does,
 * to keep the old per-owner behaviour. Free/untracked/exited bytes aren't
 * owners and never get a legend row; the map (bnu__print_layout_region())
 * paints them straight from each block with no legend bookkeeping needed. */
static size_t bnu__build_layout_legend(
    const bruce_memory_layout_block_t *blocks, size_t count, const bruce_process_snapshot_t *processes,
    size_t process_count, bool compact_reserved, size_t *out_padding_bytes, bnu__layout_legend_entry_t *legend
) {
    size_t legend_count = 0;
    size_t padding_bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
        if (!block->used) {
            if (block->owner_id == BRUCE_PROCESS_ID_INVALID) {
                /* Free - no legend row. */
            } else if (compact_reserved) {
                padding_bytes += block->size;
            } else {
                bnu__layout_legend_add_bytes(legend, &legend_count, block->owner_id, true, false, block->size);
            }
            continue;
        }
        if (!block->tracked) continue; /* Untracked - no legend row. */
        if (bnu__layout_process(processes, process_count, block->owner_id) == NULL) {
            continue; /* Exited owner - no legend row. */
        }
        bnu__layout_legend_add_bytes(
            legend, &legend_count, block->owner_id, false, block->is_stack, block->requested_size
        );
        if (block->size > block->requested_size) {
            size_t padding = block->size - block->requested_size;
            if (compact_reserved) padding_bytes += padding;
            else bnu__layout_legend_add_bytes(legend, &legend_count, block->owner_id, true, block->is_stack, padding);
        }
    }
    *out_padding_bytes = padding_bytes;
    return legend_count;
}

static int bnu__layout_compare_address(const void *left, const void *right) {
    const bruce_memory_layout_block_t *a = left;
    const bruce_memory_layout_block_t *b = right;
    if (a->address < b->address) return -1;
    if (a->address > b->address) return 1;
    return 0;
}

static const char *bnu__layout_region_name(bruce_memory_region_t region) {
    switch (region) {
        case BRUCE_MEMORY_REGION_DRAM: return "DRAM";
        case BRUCE_MEMORY_REGION_DIRAM: return "D/IRAM";
        case BRUCE_MEMORY_REGION_IRAM: return "IRAM";
        case BRUCE_MEMORY_REGION_RTC_FAST: return "RTC fast RAM";
        case BRUCE_MEMORY_REGION_RTC_SLOW: return "RTC slow RAM";
        case BRUCE_MEMORY_REGION_PSRAM: return "PSRAM";
        case BRUCE_MEMORY_REGION_SWAP: return "swap";
        default: return "unknown";
    }
}

/* Categories a map cell can be dominated by that aren't a specific live
 * owner - appended right after legend[]'s own indices (0..legend_count-1) to
 * form one flat 0..category_count-1 space (see bnu__print_layout_region()).
 *
 * OVERFLOW covers the near-impossible case of a backend with more than the
 * 52 available (owner, reserved, is-stack) combinations - legend[] has
 * nowhere left to put such a block, so its bytes still count towards a cell
 * (rather than being silently dropped from the map) under this shared
 * catch-all instead of a real owner's own letter. */
#define BNU_MEMORY_CATEGORY_PADDING 0
#define BNU_MEMORY_CATEGORY_OVERFLOW 1
#define BNU_MEMORY_CATEGORY_FREE 2
#define BNU_MEMORY_CATEGORY_UNTRACKED 3
#define BNU_MEMORY_CATEGORY_EXITED 4
#define BNU_MEMORY_PSEUDO_CATEGORIES 5

static char bnu__layout_category_symbol(
    const bnu__layout_legend_entry_t *legend, size_t legend_count, size_t category
) {
    if (category < legend_count) return legend[category].symbol;
    switch (category - legend_count) {
        case BNU_MEMORY_CATEGORY_PADDING: return '+';
        case BNU_MEMORY_CATEGORY_OVERFLOW: return '*';
        case BNU_MEMORY_CATEGORY_FREE: return '.';
        case BNU_MEMORY_CATEGORY_UNTRACKED: return '?';
        case BNU_MEMORY_CATEGORY_EXITED: return '!';
        default: return '#'; /* Unreachable: every category above is handled. */
    }
}

/* Adds [byte_start, byte_end) worth of a category's bytes into every map
 * cell it actually overlaps, weighted by exactly how many of those bytes
 * fall in each cell - not just "did this block touch this cell at all".
 * That per-cell weight is what lets bnu__print_layout_region() later paint
 * each cell as whichever single category holds the most of it, instead of
 * inventing a new symbol the moment two things share a cell. */
static void bnu__layout_add_bytes_to_cells(
    uint32_t *counts, size_t category_count, size_t category, size_t span, size_t byte_start, size_t byte_end
) {
    if (byte_end <= byte_start || span == 0) return;
    size_t first = byte_start * BNU_MEMORY_LAYOUT_WIDTH / span;
    size_t last = (byte_end * BNU_MEMORY_LAYOUT_WIDTH + span - 1) / span;
    if (last > BNU_MEMORY_LAYOUT_WIDTH) last = BNU_MEMORY_LAYOUT_WIDTH;
    for (size_t cell = first; cell < last; ++cell) {
        size_t cell_start = cell * span / BNU_MEMORY_LAYOUT_WIDTH;
        size_t cell_end = (cell + 1) * span / BNU_MEMORY_LAYOUT_WIDTH;
        size_t overlap_start = byte_start > cell_start ? byte_start : cell_start;
        size_t overlap_end = byte_end < cell_end ? byte_end : cell_end;
        if (overlap_end > overlap_start) {
            counts[cell * category_count + category] += (uint32_t)(overlap_end - overlap_start);
        }
    }
}

/* Paints one region's map: every block contributes its bytes, cell by cell,
 * to whichever category it belongs to (a specific owner's used or reserved
 * half, or one of the fixed padding/free/untracked/exited/overflow
 * categories); once every block has been counted, each cell is painted with
 * whichever single category holds the most of it. A cell too coarse to hold
 * one thing cleanly always shows *something real* - the category that
 * actually dominates it - rather than a synthetic combo letter that doesn't
 * correspond to any legend row. The legend below always lists a category's
 * true total regardless of whether it wins any cell at this resolution. */
static void bnu__print_layout_region(
    const bruce_memory_layout_block_t *blocks, size_t count,
    const bruce_process_snapshot_t *processes, size_t process_count,
    const bnu__layout_legend_entry_t *legend, size_t legend_count, bool compact_reserved, bool human,
    size_t region_number, uintptr_t region_start, uintptr_t region_end, bruce_memory_region_t region
) {
    if (region_end <= region_start) return;
    size_t span = region_end - region_start;
    char map[BNU_MEMORY_LAYOUT_WIDTH + 1];
    map[BNU_MEMORY_LAYOUT_WIDTH] = '\0';

    size_t category_count = legend_count + BNU_MEMORY_PSEUDO_CATEGORIES;
    /* Heap-allocated: up to 48 * 57 uint32_t (~10.5K) in the worst case,
     * scaled down to a few hundred bytes normally by legend_count - either
     * way, too big to risk on this process's small default stack. */
    uint32_t *counts = memory__calloc(BNU_MEMORY_LAYOUT_WIDTH * category_count, sizeof(*counts));
    if (counts == NULL) {
        stdio__printf(
            "%u. region %-12s 0x%08lx-0x%08lx map unavailable: out of memory\n", (unsigned)region_number,
            bnu__layout_region_name(region), (unsigned long)region_start, (unsigned long)region_end
        );
        return;
    }

    size_t used = 0;
    size_t largest_free = 0;
    for (size_t i = 0; i < count; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
        if (block->region_start != region_start || block->region_end != region_end) continue;
        if (block->used) used += block->size;
        else if (block->size > largest_free) largest_free = block->size;
        size_t relative = block->address > region_start ? block->address - region_start : 0;
        size_t end = relative + block->size;
        if (end > span) end = span;

        if (!block->used) {
            /* Free: nobody's holding it, RAM's compacted padding, or (swap
             * only) reserved for a specific owner - see
             * bnu__build_layout_legend(). */
            if (block->owner_id == BRUCE_PROCESS_ID_INVALID) {
                bnu__layout_add_bytes_to_cells(
                    counts, category_count, legend_count + BNU_MEMORY_CATEGORY_FREE, span, relative, end
                );
            } else if (compact_reserved) {
                bnu__layout_add_bytes_to_cells(
                    counts, category_count, legend_count + BNU_MEMORY_CATEGORY_PADDING, span, relative, end
                );
            } else {
                size_t index = bnu__layout_legend_index(legend, legend_count, block->owner_id, true, false);
                bnu__layout_add_bytes_to_cells(
                    counts, category_count,
                    index == SIZE_MAX ? legend_count + BNU_MEMORY_CATEGORY_OVERFLOW : index, span, relative, end
                );
            }
        } else if (!block->tracked) {
            bnu__layout_add_bytes_to_cells(
                counts, category_count, legend_count + BNU_MEMORY_CATEGORY_UNTRACKED, span, relative, end
            );
        } else if (bnu__layout_process(processes, process_count, block->owner_id) == NULL) {
            bnu__layout_add_bytes_to_cells(
                counts, category_count, legend_count + BNU_MEMORY_CATEGORY_EXITED, span, relative, end
            );
        } else {
            /* Split the block's own span at the boundary between bytes it
             * actually asked for (block->requested_size) and the rounded-up
             * remainder its allocator reserved alongside them
             * (block->size - block->requested_size, if any) - the used half
             * always counts towards that owner's own category; the reserved
             * half counts towards RAM's padding category (compacted) or its
             * own reserved category on swap. */
            size_t requested_end = relative + block->requested_size;
            if (requested_end > end) requested_end = end;

            size_t used_index =
                bnu__layout_legend_index(legend, legend_count, block->owner_id, false, block->is_stack);
            bnu__layout_add_bytes_to_cells(
                counts, category_count,
                used_index == SIZE_MAX ? legend_count + BNU_MEMORY_CATEGORY_OVERFLOW : used_index, span, relative,
                requested_end
            );

            if (end > requested_end) {
                if (compact_reserved) {
                    bnu__layout_add_bytes_to_cells(
                        counts, category_count, legend_count + BNU_MEMORY_CATEGORY_PADDING, span, requested_end, end
                    );
                } else {
                    size_t reserved_index =
                        bnu__layout_legend_index(legend, legend_count, block->owner_id, true, block->is_stack);
                    bnu__layout_add_bytes_to_cells(
                        counts, category_count,
                        reserved_index == SIZE_MAX ? legend_count + BNU_MEMORY_CATEGORY_OVERFLOW : reserved_index,
                        span, requested_end, end
                    );
                }
            }
        }
    }
    /* A cell shared by two or more *live processes* - as opposed to an owner
     * merely sharing a cell with free/untracked/padding noise, which its own
     * letter already covers well enough - gets a dedicated '%' instead of
     * silently picking whichever owner has the most bytes there. Detected in
     * the same pass that picks each cell's winning category, since both need
     * the same per-category byte counts. */
    bool shared[BNU_MEMORY_LAYOUT_WIDTH];
    for (size_t cell = 0; cell < BNU_MEMORY_LAYOUT_WIDTH; ++cell) {
        size_t best_category = SIZE_MAX;
        uint32_t best_bytes = 0;
        size_t owners_present = 0;
        for (size_t category = 0; category < category_count; ++category) {
            uint32_t bytes = counts[cell * category_count + category];
            if (bytes == 0) continue;
            if (category < legend_count) ++owners_present;
            if (bytes > best_bytes) {
                best_bytes = bytes;
                best_category = category;
            }
        }
        shared[cell] = owners_present >= 2;
        map[cell] = best_category == SIZE_MAX ? '-'
                    : shared[cell]             ? '%'
                                               : bnu__layout_category_symbol(legend, legend_count, best_category);
    }
    /* used can't exceed span - every contributing block->size was already
     * clamped against this same region when memory_layout__visit() built it -
     * but derive free_size defensively rather than trusting that here too. */
    size_t free_size = used < span ? span - used : 0;
    char span_text[16];
    char used_text[16];
    char free_text[16];
    char largest_text[16];
    bnu__format_size((uint32_t)span, true, span_text, sizeof(span_text));
    bnu__format_size((uint32_t)used, true, used_text, sizeof(used_text));
    bnu__format_size((uint32_t)free_size, true, free_text, sizeof(free_text));
    bnu__format_size((uint32_t)largest_free, true, largest_text, sizeof(largest_text));
    stdio__printf(
        "%u. region %-12s 0x%08lx-0x%08lx (%s/%s) free: %s lrgst: %s\n[%s]\n", (unsigned)region_number,
        bnu__layout_region_name(region),
        (unsigned long)region_start, (unsigned long)region_end, used_text, span_text, free_text, largest_text, map
    );
    /* One line per '%' cell, breaking down exactly what's in *that cell* -
     * not a backend-wide total - so distinct '%' cells never repeat the same
     * numbers at each other the way the old combo rows did. Column is
     * 1-based to match counting characters into the bracketed map above. */
    for (size_t cell = 0; cell < BNU_MEMORY_LAYOUT_WIDTH; ++cell) {
        if (!shared[cell]) continue;
        stdio__printf("  %% col %u:", (unsigned)(cell + 1));
        bool first_owner = true;
        for (size_t category = 0; category < legend_count; ++category) {
            uint32_t bytes = counts[cell * category_count + category];
            if (bytes == 0) continue;
            const bruce_process_snapshot_t *process =
                bnu__layout_process(processes, process_count, legend[category].owner_id);
            char bytes_text[16];
            bnu__format_size(bytes, human, bytes_text, sizeof(bytes_text));
            stdio__printf(
                "%s %u %s%s %s", first_owner ? "" : ",", (unsigned)legend[category].owner_id,
                process != NULL ? process->name : "<exited>", legend[category].is_stack ? " (stack)" : "",
                bytes_text
            );
            first_owner = false;
        }
        stdio__printf("\n");
    }
    memory__free(counts);
}

static void bnu__print_layout_backend(
    const char *name, bruce_memory_backend_t backend, const bruce_process_snapshot_t *processes,
    size_t process_count, bool human, bruce_memory_layout_block_t *blocks, size_t capacity
) {
    size_t count = 0;
    bruce_result_t result = memory__get_layout(backend, blocks, capacity, &count);
    if (result != BRUCE_OK) {
        stdio__printf("\n%s layout unavailable: %s\n", name, result__to_string(result));
        return;
    }
    size_t shown = count < capacity ? count : capacity;
    qsort(blocks, shown, sizeof(*blocks), bnu__layout_compare_address);
    size_t total = 0;
    for (size_t i = 0; i < shown; ++i) total += blocks[i].size;
    if (total == 0) return;

    /* Heap-allocated for the same reason as processes[] below - this and the
     * legend array together would otherwise add ~2.5K to a process that
     * normally runs on a default-sized stack. */
    bnu__layout_legend_entry_t *legend = memory__malloc(BNU_MEMORY_LEGEND_MAX * sizeof(*legend));
    if (legend == NULL) {
        stdio__printf("\n%s layout unavailable: out of memory\n", name);
        return;
    }
    /* RAM's own padding is never worth splitting out per owner - see
     * BNU_MEMORY_LEGEND_MAX's doc comment - so only swap keeps the detailed
     * per-owner "reserved" accounting the map/legend otherwise show. */
    bool compact_reserved = backend != BRUCE_MEMORY_BACKEND_SWAP;
    size_t padding_bytes = 0;
    size_t legend_count =
        bnu__build_layout_legend(blocks, shown, processes, process_count, compact_reserved, &padding_bytes, legend);

    char total_text[16];
    bnu__format_size((uint32_t)total, true, total_text, sizeof(total_text));
    stdio__printf("\n%s layout (%s, physical regions)\n", name, total_text);
    uintptr_t previous_start = UINTPTR_MAX;
    uintptr_t previous_end = UINTPTR_MAX;
    size_t region_number = 0;
    for (size_t i = 0; i < shown; ++i) {
        if (blocks[i].region_start == previous_start && blocks[i].region_end == previous_end) continue;
        ++region_number;
        bnu__print_layout_region(
            blocks, shown, processes, process_count, legend, legend_count, compact_reserved, human, region_number,
            blocks[i].region_start, blocks[i].region_end, blocks[i].region
        );
        previous_start = blocks[i].region_start;
        previous_end = blocks[i].region_end;
    }
    stdio__printf(
        ". free  ? untracked  ! exited owner  - allocator metadata  %% shared by processes (see below)"
        "  # too many processes to letter%s\n",
        compact_reserved ? "  + padding" : ""
    );
    for (size_t i = 0; i < legend_count; ++i) {
        const bruce_process_snapshot_t *process = bnu__layout_process(processes, process_count, legend[i].owner_id);
        /* Sized for the worst case (BRUCE_PROCESS_NAME_MAX plus " (stack)")
         * so the compiler's format-truncation check can prove this always
         * fits - %-15.15s below truncates the actually-printed name anyway. */
        char label[80];
        if (legend[i].is_stack) {
            snprintf(label, sizeof(label), "%s (stack)", process != NULL ? process->name : "<exited>");
        } else {
            snprintf(label, sizeof(label), "%s", process != NULL ? process->name : "<exited>");
        }
        char bytes_text[16];
        bnu__format_size((uint32_t)legend[i].bytes, human, bytes_text, sizeof(bytes_text));
        /* RAM rows are always "used" now that reserved bytes never get a
         * per-owner entry there (see BNU_MEMORY_LEGEND_MAX's doc comment) -
         * printing that word on every single row would say nothing. Swap
         * still distinguishes the two, so it keeps the column. */
        if (compact_reserved) {
            stdio__printf(
                "%c pid %-3u %-15.15s %8s\n", legend[i].symbol, (unsigned)legend[i].owner_id, label, bytes_text
            );
        } else {
            stdio__printf(
                "%c pid %-3u %-15.15s %-8s %8s\n", legend[i].symbol, (unsigned)legend[i].owner_id, label,
                legend[i].reserved ? "reserved" : "used", bytes_text
            );
        }
    }
    if (compact_reserved && padding_bytes > 0) {
        char bytes_text[16];
        bnu__format_size((uint32_t)padding_bytes, human, bytes_text, sizeof(bytes_text));
        stdio__printf("%c pid %-3s %-15.15s %-8s %8s\n", '+', "-", "(padding)", "reserved", bytes_text);
    }

    /* "rgn" cross-references each row against the numbered region headers
     * above (see bnu__print_layout_region()) - counted the same way, over
     * the same address-sorted blocks, so the numbers always line up.
     * RAM shows "type" (heap/stack) here instead of "state": every row this
     * loop prints for a RAM backend is already used (untracked and free
     * blocks are filtered out below), so "state" would say "used" on every
     * single line - swap has no such filter, so it keeps the more useful
     * used/free distinction. */
    stdio__printf(
        "%-10s %-3s %-5s %-4s %-15s %8s %8s\n", "address", "rgn", compact_reserved ? "type" : "state", "pid",
        "owner", "request", "block"
    );
    region_number = 0;
    previous_start = UINTPTR_MAX;
    previous_end = UINTPTR_MAX;
    for (size_t i = 0; i < shown; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
        if (block->region_start != previous_start || block->region_end != previous_end) {
            ++region_number;
            previous_start = block->region_start;
            previous_end = block->region_end;
        }
        if (backend != BRUCE_MEMORY_BACKEND_SWAP && !block->tracked) continue;
        const bruce_process_snapshot_t *process =
            bnu__layout_process(processes, process_count, block->owner_id);
        char requested[16] = "-";
        char reserved[16];
        if (block->used) {
            bnu__format_size((uint32_t)block->requested_size, human, requested, sizeof(requested));
        }
        bnu__format_size((uint32_t)block->size, human, reserved, sizeof(reserved));
        stdio__printf(
            "0x%08lx %-3u %-5s %-4u %-15.15s %8s %8s%s\n",
            (unsigned long)block->address, (unsigned)region_number,
            compact_reserved ? (block->is_stack ? "stack" : "heap") : (block->used ? "used" : "free"),
            (unsigned)block->owner_id,
            process != NULL ? process->name
            : block->used || block->owner_id != BRUCE_PROCESS_ID_INVALID ? "<exited>"
                                                                          : "-",
            requested, reserved, block->executable ? " xip" : ""
        );
    }
    if (count > shown) {
        stdio__printf("... %u blocks omitted (snapshot limit)\n", (unsigned)(count - shown));
    }
    memory__free(legend);
}

/* Task stacks are FreeRTOS-allocated, not something memory__get_layout()'s
 * heap-block walk attributes to a process, so this reads straight off the
 * process snapshot (the same stack_total_bytes/stack_high_water_bytes `top`
 * already reports) instead of needing a second allocator pass. */
static void bnu__print_layout_stacks(
    const bruce_process_snapshot_t *processes, size_t process_count, bool human
) {
    stdio__printf("\nprocess stacks\n");
    stdio__printf("%-4s %-15s %8s %8s %8s\n", "pid", "name", "stack", "used", "free");
    for (size_t i = 0; i < process_count; ++i) {
        uint32_t used = processes[i].stack_total_bytes > processes[i].stack_high_water_bytes
                            ? processes[i].stack_total_bytes - processes[i].stack_high_water_bytes
                            : 0;
        char stack_text[16];
        char used_text[16];
        char free_text[16];
        bnu__format_size(processes[i].stack_total_bytes, human, stack_text, sizeof(stack_text));
        bnu__format_size(used, human, used_text, sizeof(used_text));
        bnu__format_size(processes[i].stack_high_water_bytes, human, free_text, sizeof(free_text));
        stdio__printf(
            "%-4u %-15.15s %8s %8s %8s\n", (unsigned)processes[i].id, processes[i].name, stack_text, used_text,
            free_text
        );
    }
}

static void
bnu__print_memory_row(const char *name, size_t total, size_t free_size, size_t largest, bool human) {
    char total_text[16];
    char used_text[16];
    char free_text[16];
    char largest_text[16];
    bnu__format_size((uint32_t)total, human, total_text, sizeof(total_text));
    bnu__format_size((uint32_t)(total - free_size), human, used_text, sizeof(used_text));
    bnu__format_size((uint32_t)free_size, human, free_text, sizeof(free_text));
    bnu__format_size((uint32_t)largest, human, largest_text, sizeof(largest_text));
    stdio__printf("%-5s %7s %7s %6s %6s\n", name, total_text, used_text, free_text, largest_text);
}

int bnu_free_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show internal memory, PSRAM, and swap usage.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "h");
    ap_set_opt_help(parser, "h", "Show sizes in human-readable units (e.g. 8.2K, 1.3M)");
    ap_add_flag(parser, "m");
    ap_set_opt_help(parser, "m", "Show proportional allocator maps and tracked owners");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool human = ap_found(parser, "h");
    bool show_map = ap_found(parser, "m");
    ap_free(parser);
    bruce_memory_stats_t stats;
    bruce_result_t result = memory__get_stats(&stats);
    if (result != BRUCE_OK) return result;
    stdio__printf("%-5s %7s %7s %6s %6s\n", "mem", "total", "used", "free", "lrgst");
    bnu__print_memory_row(
        "int", stats.internal_total, stats.internal_free, stats.internal_largest_block, human
    );
    if (stats.psram_total > 0) {
        bnu__print_memory_row("psram", stats.psram_total, stats.psram_free, stats.psram_largest_block, human);
    }
    if (stats.swap_total > 0) {
        bnu__print_memory_row("swap", stats.swap_total, stats.swap_free, stats.swap_largest_block, human);
    }
    if (show_map) {
        const bruce_memory_backend_t backends[] = {
            BRUCE_MEMORY_BACKEND_INTERNAL,
            BRUCE_MEMORY_BACKEND_PSRAM,
            BRUCE_MEMORY_BACKEND_SWAP,
        };
        size_t capacity = 0;
        for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); ++i) {
            if ((backends[i] == BRUCE_MEMORY_BACKEND_PSRAM && stats.psram_total == 0) ||
                (backends[i] == BRUCE_MEMORY_BACKEND_SWAP && stats.swap_total == 0)) {
                continue;
            }
            size_t required = 0;
            result = memory__get_layout(backends[i], NULL, 0, &required);
            if (result != BRUCE_OK) {
                stdio__printf("free: -m layout unavailable: %s\n", result__to_string(result));
                return result;
            }
            if (required > capacity) capacity = required;
        }
        /* The temporary snapshot itself can add one block between the
         * counting and capture passes (to whichever backend it ends up
         * allocated from). Leave a little room for concurrent allocator
         * activity without keeping any permanent BSS reservation. */
        if (capacity > SIZE_MAX - 4) {
            stdio__printf("free: -m layout unavailable: block count too large\n");
            return BRUCE_ERR_NO_MEMORY;
        }
        capacity += 4;
        if (capacity > SIZE_MAX / sizeof(bruce_memory_layout_block_t)) {
            stdio__printf("free: -m layout unavailable: block count too large\n");
            return BRUCE_ERR_NO_MEMORY;
        }
        /* A live heap this snapshot walks can hold vastly more blocks than a
         * single fixed-size buffer will ever comfortably fit (a heavily
         * fragmented, tens-of-MB heap can have hundreds of thousands) - a
         * one-shot allocation sized to the *exact* live count is prone
         * to failing outright on exactly the busy systems this is meant to
         * diagnose. Cap it at a size that always fits a normal heap and rely
         * on bnu__print_layout_backend()'s own "N blocks omitted" truncation
         * (already handles a *_get_layout() count exceeding capacity) rather
         * than needing every single block to fit at once. */
        static const size_t BNU_FREE_MAP_MAX_BLOCKS = 8192;
        if (capacity > BNU_FREE_MAP_MAX_BLOCKS) capacity = BNU_FREE_MAP_MAX_BLOCKS;
        /* This is a scratch buffer, not something worth taking bytes away
         * from the internal heap for - the whole point of `free -m` is to
         * run when internal RAM is already tight. Prefer PSRAM (never
         * swap: memory__get_layout() below writes into this buffer through
         * an ordinary pointer, which a flash-backed swap allocation
         * wouldn't survive) and only fall back to memory__malloc() if
         * there's no PSRAM or it's full too.
         *
         * On a board with no PSRAM at all (memory__external_malloc_writable()
         * fails immediately, every time), that fallback has to come out of
         * the very internal heap this snapshot is reporting on - so a heap
         * fragmented enough to be worth diagnosing can also be exactly the
         * one that can't produce one contiguous buffer sized for its own
         * full (even capped) block count. Retry at half the size, down to a
         * floor that still shows something, rather than giving up with
         * nothing: bnu__print_layout_backend()'s own truncation ("N blocks
         * omitted") already handles a get_layout() count exceeding whatever
         * capacity actually fit. */
        static const size_t BNU_FREE_MAP_MIN_BLOCKS = 64;
        bool blocks_external = true;
        bruce_memory_layout_block_t *blocks = NULL;
        while (true) {
            blocks_external = true;
            blocks = (bruce_memory_layout_block_t *)memory__external_malloc_writable(capacity * sizeof(*blocks));
            if (blocks == NULL) {
                blocks_external = false;
                blocks = memory__malloc(capacity * sizeof(*blocks));
            }
            if (blocks != NULL || capacity <= BNU_FREE_MAP_MIN_BLOCKS) break;
            capacity = capacity / 2 > BNU_FREE_MAP_MIN_BLOCKS ? capacity / 2 : BNU_FREE_MAP_MIN_BLOCKS;
        }
        if (blocks == NULL) {
            stdio__printf("free: -m layout unavailable: out of memory (%zu blocks)\n", capacity);
            return BRUCE_ERR_NO_MEMORY;
        }
        bruce_process_snapshot_t *processes = memory__malloc(BNU_PROCESS_SNAPSHOT_MAX * sizeof(*processes));
        if (processes == NULL) {
            stdio__printf("free: -m layout unavailable: out of memory\n");
            if (blocks_external) memory__external_free(blocks);
            else memory__free(blocks);
            return BRUCE_ERR_NO_MEMORY;
        }
        size_t process_count = 0;
        result = process__list(processes, BNU_PROCESS_SNAPSHOT_MAX, &process_count);
        if (result != BRUCE_OK) {
            stdio__printf("free: -m layout unavailable: %s\n", result__to_string(result));
            memory__free(processes);
            if (blocks_external) memory__external_free(blocks);
            else memory__free(blocks);
            return result;
        }
        if (process_count > BNU_PROCESS_SNAPSHOT_MAX) { process_count = BNU_PROCESS_SNAPSHOT_MAX; }
        bnu__print_layout_backend(
            "internal", BRUCE_MEMORY_BACKEND_INTERNAL, processes, process_count, human, blocks, capacity
        );
        if (stats.psram_total > 0) {
            bnu__print_layout_backend(
                "psram", BRUCE_MEMORY_BACKEND_PSRAM, processes, process_count, human, blocks, capacity
            );
        }
        if (stats.swap_total > 0) {
            bnu__print_layout_backend(
                "swap", BRUCE_MEMORY_BACKEND_SWAP, processes, process_count, human, blocks, capacity
            );
        }
        if (blocks_external) memory__external_free(blocks);
        else memory__free(blocks);
        bnu__print_layout_stacks(processes, process_count, human);
        memory__free(processes);
    }
    return BRUCE_OK;
}

static const char *bnu__process_state_name(bruce_process_state_t state) {
    switch (state) {
        case BRUCE_PROCESS_STARTING: return "start";
        case BRUCE_PROCESS_FOREGROUND: return "fore";
        case BRUCE_PROCESS_BACKGROUND: return "back";
        case BRUCE_PROCESS_PAUSED: return "pause";
        case BRUCE_PROCESS_STOPPING: return "stop";
        default: return "?";
    }
}

int bnu_top_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show runtime process resource usage.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "h");
    ap_set_opt_help(parser, "h", "Show stck/heap/swap sizes in human-readable units (e.g. 8.2K, 1.3M)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool human = ap_found(parser, "h");
    ap_free(parser);

    bruce_process_snapshot_t *processes = memory__malloc(BNU_PROCESS_SNAPSHOT_MAX * sizeof(*processes));
    if (processes == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t process_count = 0;
    bruce_result_t result = process__list(processes, BNU_PROCESS_SNAPSHOT_MAX, &process_count);
    if (result != BRUCE_OK) {
        memory__free(processes);
        return result;
    }
    result = runtime__delay(250);
    if (result != BRUCE_OK) {
        memory__free(processes);
        return result;
    }
    result = process__list(processes, BNU_PROCESS_SNAPSHOT_MAX, &process_count);
    if (result != BRUCE_OK) {
        memory__free(processes);
        return result;
    }

    stdio__printf("\n%1s %2s %3s %4s %4s %4s %s\n", "s", "id", "cpu", "stck", "heap", "swap", "name");
    for (size_t i = 0; i < process_count; ++i) {
        uint32_t stack_used_bytes = processes[i].stack_total_bytes > processes[i].stack_high_water_bytes
                                        ? processes[i].stack_total_bytes - processes[i].stack_high_water_bytes
                                        : 0;
        /* memory_bytes tracks everything the process owns, swap included;
         * subtract swap_bytes here so the displayed "heap" is RAM only
         * (internal heap + PSRAM) and doesn't double-count the swap column. */
        uint32_t ram_bytes = processes[i].memory_bytes > processes[i].swap_bytes
                                 ? processes[i].memory_bytes - processes[i].swap_bytes
                                 : 0;
        char stack_text[16];
        char heap_text[16];
        char swap_text[16];
        bnu__format_size(stack_used_bytes, human, stack_text, sizeof(stack_text));
        bnu__format_size(ram_bytes, human, heap_text, sizeof(heap_text));
        bnu__format_size((uint32_t)processes[i].swap_bytes, human, swap_text, sizeof(swap_text));
        stdio__printf(
            "%1.1s %2u %3u %4s %4s %4s %.15s\n",
            bnu__process_state_name(processes[i].state),
            (unsigned)processes[i].id,
            (unsigned)processes[i].cpu_percent,
            stack_text,
            heap_text,
            swap_text,
            processes[i].name
        );
    }
    memory__free(processes);
    return BRUCE_OK;
}

static bruce_result_t bnu__parse_shutdown_time(const char *text, uint32_t *out_delay_ms) {
    if (text == NULL || out_delay_ms == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (strcmp(text, "now") == 0) {
        *out_delay_ms = 0;
        return BRUCE_OK;
    }

    if (text[0] == '+') {
        errno = 0;
        char *end = NULL;
        unsigned long minutes = strtoul(text + 1, &end, 10);
        if (errno != 0 || end == text + 1 || *end != '\0' || minutes > UINT32_MAX / 60000u) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        *out_delay_ms = (uint32_t)minutes * 60000u;
        return BRUCE_OK;
    }

    if (strlen(text) != 5 || text[2] != ':' || text[0] < '0' || text[0] > '9' || text[1] < '0' ||
        text[1] > '9' || text[3] < '0' || text[3] > '9' || text[4] < '0' || text[4] > '9') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    unsigned hour = (unsigned)(text[0] - '0') * 10u + (unsigned)(text[1] - '0');
    unsigned minute = (unsigned)(text[3] - '0') * 10u + (unsigned)(text[4] - '0');
    if (hour > 23 || minute > 59) return BRUCE_ERR_INVALID_ARGUMENT;

    bruce_clock_datetime_t now;
    bruce_result_t result = clock__get_local(&now);
    if (result != BRUCE_OK) return result;
    int delay_minutes = (int)(hour * 60u + minute) - (int)(now.hour * 60u + now.minute);
    if (delay_minutes <= 0) delay_minutes += 24 * 60;
    *out_delay_ms = (uint32_t)delay_minutes * 60000u;
    return BRUCE_OK;
}

int bnu_shutdown_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Power off the device at the specified time.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "time", "'now', '+minutes', or 24-hour 'HH:MM'");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    uint32_t delay_ms = 0;
    bruce_result_t result = bnu__parse_shutdown_time(ap_get_arg(parser, "time"), &delay_ms);
    ap_free(parser);
    if (result != BRUCE_OK) return result;
    stdio__printf("Shutting down...\n");
    return device__power_off(delay_ms);
}

int bnu_reboot_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Restart the device.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);
    stdio__printf("Rebooting...\n");
    return device__restart(0);
}

int bnu_stty_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser(
        "Show or change the calling process's terminal settings (rows, columns, raw/cooked mode)."
    );
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_optional_arg(parser, "setting", "'size', 'raw', or '-raw'/'cooked'/'sane'");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *setting = ap_get_arg(parser, "setting");
    ap_free(parser);

    if (!tty__isatty()) {
        stdio__printf("stty: standard input is not a tty\n");
        return BRUCE_ERR_NOT_FOUND;
    }

    if (setting == NULL) {
        bruce_tty_size_t size;
        bruce_result_t result = tty__get_size(&size);
        if (result != BRUCE_OK) return result;
        stdio__printf(
            "speed 0 baud; rows %u; columns %u; line = 0;\n%s\n",
            (unsigned)size.rows,
            (unsigned)size.columns,
            tty__get_mode() == BRUCE_TTY_MODE_RAW ? "raw -echo -icanon" : "-raw echo icanon"
        );
        return BRUCE_OK;
    }
    if (strcmp(setting, "size") == 0) {
        bruce_tty_size_t size;
        bruce_result_t result = tty__get_size(&size);
        if (result != BRUCE_OK) return result;
        stdio__printf("%u %u\n", (unsigned)size.rows, (unsigned)size.columns);
        return BRUCE_OK;
    }
    if (strcmp(setting, "raw") == 0) return tty__set_mode(BRUCE_TTY_MODE_RAW);
    if (strcmp(setting, "-raw") == 0 || strcmp(setting, "cooked") == 0 || strcmp(setting, "sane") == 0) {
        return tty__set_mode(BRUCE_TTY_MODE_COOKED);
    }
    stdio__printf("stty: unknown setting '%s'\n", setting);
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static bool bnu__parse_datetime(const char *text, bruce_clock_datetime_t *out) {
    unsigned int year, month, day, hour, minute, second;
    char extra;
    if (text == NULL ||
        sscanf(text, "%u-%u-%u %u:%u:%u%c", &year, &month, &day, &hour, &minute, &second, &extra) != 6) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    out->year = (uint16_t)year;
    out->month = (uint8_t)month;
    out->day = (uint8_t)day;
    out->hour = (uint8_t)hour;
    out->minute = (uint8_t)minute;
    out->second = (uint8_t)second;
    return true;
}

int bnu_date_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show or set the current date and time.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "u");
    ap_set_opt_help(parser, "u", "Show the time in UTC instead of local time");
    ap_add_str_opt(parser, "s set", NULL);
    ap_set_opt_help(parser, "s set", "Set the date and time ('YYYY-MM-DD HH:MM:SS', local)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    bool utc = ap_found(parser, "u");
    const char *set_value = ap_get_str_value(parser, "s");
    bruce_clock_datetime_t set_local;
    bool has_set_value = set_value != NULL && bnu__parse_datetime(set_value, &set_local);
    bool set_requested = set_value != NULL;
    ap_free(parser);

    if (set_requested) {
        if (!has_set_value) return BRUCE_ERR_INVALID_ARGUMENT;
        bruce_result_t result = clock__set_local(&set_local);
        if (result != BRUCE_OK) return result;
    }

    bruce_clock_datetime_t now;
    bruce_result_t result = utc ? clock__get_utc(&now) : clock__get_local(&now);
    if (result != BRUCE_OK) return result;
    stdio__printf(
        "%04u-%02u-%02u %02u:%02u:%02u%s\n",
        (unsigned)now.year,
        (unsigned)now.month,
        (unsigned)now.day,
        (unsigned)now.hour,
        (unsigned)now.minute,
        (unsigned)now.second,
        utc ? " UTC" : ""
    );
    return BRUCE_OK;
}

int bnu_sleep_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Pause for the given duration.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "seconds", "Duration to sleep, in seconds (e.g. 2 or 0.5)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *text = ap_get_arg(parser, "seconds");

    errno = 0;
    char *end = NULL;
    double seconds = text != NULL ? strtod(text, &end) : 0.0;
    bool valid = text != NULL && end != text && *end == '\0' && errno == 0 && seconds >= 0.0 &&
                 seconds <= (double)UINT32_MAX / 1000.0;
    ap_free(parser);
    if (!valid) return BRUCE_ERR_INVALID_ARGUMENT;

    return runtime__sleep((uint32_t)(seconds * 1000.0));
}
