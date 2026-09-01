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

/* Distinct sets of bits (legend[] owners and/or the fixed BNU_MEMORY_BIT_*
 * categories - see bnu__layout_mark()) observed sharing a single map cell.
 * Resolved only after every block in a region has been marked (see
 * bnu__layout_resolve_symbol()), so anything sharing a cell - two owners, an
 * owner and some untracked heap noise, whatever - gets its own dedicated
 * letter and breakdown line instead of collapsing straight to the generic
 * '#' the instant it first overlaps with something else. Sized generously:
 * unlike the old owner-only combos (rare), a dense heap region can genuinely
 * have this many distinct overlaps once free/untracked/padding are counted
 * too - '#' is now reserved for the rarer case of exhausting even this. */
#define BNU_MEMORY_COMBO_MAX 24

typedef struct {
    uint64_t members; /* bitmask over legend[] indices and BNU_MEMORY_BIT_* */
    char symbol;
} bnu__layout_combo_entry_t;

/* Index counterpart of the old per-symbol lookup: building the per-cell
 * membership bitmask bnu__layout_resolve_symbol() later turns into a symbol
 * needs legend[]'s *index*, not its already-assigned letter. SIZE_MAX on the
 * same "ran out of the 52 available letters" overflow the previous flat
 * per-process assignment used (callers fall back to '*', same as before). */
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

/* Backend-wide totals for everything that ISN'T a specific live owner - each
 * gets a fixed bit position (see BNU_MEMORY_BIT_FREE et al.) rather than a
 * legend[] slot, since there's exactly one of each per backend rather than
 * one per owner. Reused wherever one of these ends up as a member of a
 * combo cell (bnu__layout_resolve_symbol()), the same way a real owner's
 * legend[].bytes gets reused there. */
typedef struct {
    size_t padding_bytes; /* RAM only - see compact_reserved. */
    size_t free_bytes;
    size_t untracked_bytes;
    size_t exited_bytes;
} bnu__layout_totals_t;

/* Scans every block this backend reported (not just one region) so the same
 * owner gets the same letter in every region's map, and assigns letters in
 * address order (blocks are already sorted by the time this runs) - the
 * closest thing to "in the order you'd read the maps" without tracking each
 * region separately.
 *
 * compact_reserved folds every "reserved" byte (regardless of owner) into
 * out_totals->padding_bytes instead of a per-owner legend entry - see
 * BNU_MEMORY_LEGEND_MAX's doc comment. Pass false, as the swap backend does,
 * to keep the old per-owner behaviour. */
static size_t bnu__build_layout_legend(
    const bruce_memory_layout_block_t *blocks, size_t count, const bruce_process_snapshot_t *processes,
    size_t process_count, bool compact_reserved, bnu__layout_totals_t *out_totals,
    bnu__layout_legend_entry_t *legend
) {
    size_t legend_count = 0;
    bnu__layout_totals_t totals = {0};
    for (size_t i = 0; i < count; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
        if (!block->used) {
            if (block->owner_id == BRUCE_PROCESS_ID_INVALID) {
                totals.free_bytes += block->size;
            } else if (compact_reserved) {
                totals.padding_bytes += block->size;
            } else {
                bnu__layout_legend_add_bytes(legend, &legend_count, block->owner_id, true, false, block->size);
            }
            continue;
        }
        if (!block->tracked) {
            totals.untracked_bytes += block->size;
            continue;
        }
        if (bnu__layout_process(processes, process_count, block->owner_id) == NULL) {
            totals.exited_bytes += block->size;
            continue;
        }
        bnu__layout_legend_add_bytes(
            legend, &legend_count, block->owner_id, false, block->is_stack, block->requested_size
        );
        if (block->size > block->requested_size) {
            size_t padding = block->size - block->requested_size;
            if (compact_reserved) totals.padding_bytes += padding;
            else bnu__layout_legend_add_bytes(legend, &legend_count, block->owner_id, true, block->is_stack, padding);
        }
    }
    *out_totals = totals;
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
        case BRUCE_MEMORY_REGION_PSRAM: return "PSRAM";
        case BRUCE_MEMORY_REGION_SWAP: return "swap";
        default: return "unknown";
    }
}

/* Bit positions in a cell's membership bitmask for categories that aren't a
 * specific live owner - kept well clear of legend[]'s own index range (bits
 * 0..BNU_MEMORY_LEGEND_MAX-1, i.e. 0..51) so a cell can mix owners and these
 * in the same combo without bit collisions.
 *
 * OVERFLOW covers the near-impossible case of a backend with more than the
 * 52 available (owner, reserved, is-stack) combinations - legend[] has
 * nowhere left to put such a block, so it's grouped here instead of being
 * silently dropped from the map. */
#define BNU_MEMORY_BIT_PADDING 59
#define BNU_MEMORY_BIT_OVERFLOW 60
#define BNU_MEMORY_BIT_FREE 61
#define BNU_MEMORY_BIT_UNTRACKED 62
#define BNU_MEMORY_BIT_EXITED 63

/* OR's a bit into every cell in [first, last) - shared by both a specific
 * owner's legend[] index and the fixed BNU_MEMORY_BIT_* categories. Doesn't
 * resolve a display symbol itself: what a cell ends up showing depends on
 * every bit it accumulates across the whole region, which isn't known until
 * every block has been processed (see bnu__layout_resolve_symbol()). */
static void bnu__layout_mark(uint64_t *cell_members, size_t first, size_t last, size_t bit) {
    uint64_t mask = 1ull << bit;
    for (size_t cell = first; cell < last; ++cell) cell_members[cell] |= mask;
}

static char bnu__layout_bit_symbol(const bnu__layout_legend_entry_t *legend, size_t legend_count, size_t bit) {
    switch (bit) {
        case BNU_MEMORY_BIT_PADDING: return '+';
        case BNU_MEMORY_BIT_OVERFLOW: return '*';
        case BNU_MEMORY_BIT_FREE: return '.';
        case BNU_MEMORY_BIT_UNTRACKED: return '?';
        case BNU_MEMORY_BIT_EXITED: return '!';
        default: return bit < legend_count ? legend[bit].symbol : '#';
    }
}

/* Turns a cell's accumulated membership bitmask into a display symbol: a
 * single bit's own stable symbol when it's alone, otherwise a dedicated
 * combo letter - reused whenever the exact same set of bits shares another
 * cell, continuing the same A-Z/a-z sequence right after legend[]'s own
 * letters. This is what replaces the old flat '#' for "more than one thing
 * here": every combination - two owners, an owner and some untracked heap
 * noise, free space butting up against padding, whatever - gets its own
 * letter and its own breakdown line (see bnu__print_layout_backend()) rather
 * than being flattened to one uninformative symbol. '#' now only means
 * every available letter (52 total, owners and combos combined) is spoken
 * for, or the small fixed combo table (BNU_MEMORY_COMBO_MAX) is full - both
 * rare in practice. */
static char bnu__layout_resolve_symbol(
    const bnu__layout_legend_entry_t *legend, size_t legend_count, bnu__layout_combo_entry_t *combo,
    size_t *combo_count, uint64_t members
) {
    size_t popcount = 0;
    size_t only_bit = 0;
    for (size_t bit = 0; bit < 64 && popcount <= 1; ++bit) {
        if ((members & (1ull << bit)) == 0) continue;
        only_bit = bit;
        ++popcount;
    }
    if (popcount == 0) return '-'; /* Shouldn't happen - caller skips empty cells. */
    if (popcount == 1) return bnu__layout_bit_symbol(legend, legend_count, only_bit);

    for (size_t i = 0; i < *combo_count; ++i) {
        if (combo[i].members == members) return combo[i].symbol;
    }
    size_t next_symbol_index = legend_count + *combo_count;
    if (*combo_count >= BNU_MEMORY_COMBO_MAX || next_symbol_index >= 52) return '#';
    size_t index = (*combo_count)++;
    combo[index].members = members;
    combo[index].symbol =
        next_symbol_index < 26 ? (char)('A' + next_symbol_index) : (char)('a' + (next_symbol_index - 26));
    return combo[index].symbol;
}

static void bnu__print_layout_region(
    const bruce_memory_layout_block_t *blocks, size_t count,
    const bruce_process_snapshot_t *processes, size_t process_count,
    const bnu__layout_legend_entry_t *legend, size_t legend_count, bool compact_reserved,
    bnu__layout_combo_entry_t *combo, size_t *combo_count, size_t region_number,
    uintptr_t region_start, uintptr_t region_end, bruce_memory_region_t region
) {
    if (region_end <= region_start) return;
    size_t span = region_end - region_start;
    char map[BNU_MEMORY_LAYOUT_WIDTH + 1];
    map[BNU_MEMORY_LAYOUT_WIDTH] = '\0';
    /* Per-cell membership, resolved into a display symbol only once every
     * block below has been marked - see bnu__layout_mark() /
     * bnu__layout_resolve_symbol(). Small and fixed-size (48 cells), unlike
     * the per-process/legend buffers this file already keeps on the heap -
     * safe to leave on the stack. */
    uint64_t cell_members[BNU_MEMORY_LAYOUT_WIDTH] = {0};
    size_t used = 0;
    size_t largest_free = 0;
    for (size_t i = 0; i < count; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
        if (block->region_start != region_start || block->region_end != region_end) continue;
        if (block->used) used += block->size;
        else if (block->size > largest_free) largest_free = block->size;
        size_t relative = block->address > region_start ? block->address - region_start : 0;
        size_t first = relative * BNU_MEMORY_LAYOUT_WIDTH / span;
        size_t end = relative + block->size;
        if (end > span) end = span;
        size_t last = (end * BNU_MEMORY_LAYOUT_WIDTH + span - 1) / span;
        if (last > BNU_MEMORY_LAYOUT_WIDTH) last = BNU_MEMORY_LAYOUT_WIDTH;

        if (!block->used) {
            /* Free: nobody's holding it, RAM's compacted padding, or (swap
             * only) reserved for a specific owner - see
             * bnu__build_layout_legend(). */
            if (block->owner_id == BRUCE_PROCESS_ID_INVALID) {
                bnu__layout_mark(cell_members, first, last, BNU_MEMORY_BIT_FREE);
            } else if (compact_reserved) {
                bnu__layout_mark(cell_members, first, last, BNU_MEMORY_BIT_PADDING);
            } else {
                size_t index = bnu__layout_legend_index(legend, legend_count, block->owner_id, true, false);
                bnu__layout_mark(cell_members, first, last, index == SIZE_MAX ? BNU_MEMORY_BIT_OVERFLOW : index);
            }
        } else if (!block->tracked) {
            bnu__layout_mark(cell_members, first, last, BNU_MEMORY_BIT_UNTRACKED);
        } else if (bnu__layout_process(processes, process_count, block->owner_id) == NULL) {
            bnu__layout_mark(cell_members, first, last, BNU_MEMORY_BIT_EXITED);
        } else {
            /* Split the block's own span at the boundary between bytes it
             * actually asked for (block->requested_size) and the rounded-up
             * remainder its allocator reserved alongside them
             * (block->size - block->requested_size, if any) - the used half
             * always gets that owner's own bit; the reserved half gets
             * RAM's padding bit (compacted) or its own reserved bit on
             * swap. */
            size_t requested_end = relative + block->requested_size;
            if (requested_end > end) requested_end = end;
            size_t used_last = (requested_end * BNU_MEMORY_LAYOUT_WIDTH + span - 1) / span;
            if (used_last > last) used_last = last;
            if (used_last < first) used_last = first;

            size_t used_index =
                bnu__layout_legend_index(legend, legend_count, block->owner_id, false, block->is_stack);
            bnu__layout_mark(
                cell_members, first, used_last, used_index == SIZE_MAX ? BNU_MEMORY_BIT_OVERFLOW : used_index
            );

            if (last > used_last) {
                if (compact_reserved) {
                    bnu__layout_mark(cell_members, used_last, last, BNU_MEMORY_BIT_PADDING);
                } else {
                    size_t reserved_index =
                        bnu__layout_legend_index(legend, legend_count, block->owner_id, true, block->is_stack);
                    bnu__layout_mark(
                        cell_members, used_last, last,
                        reserved_index == SIZE_MAX ? BNU_MEMORY_BIT_OVERFLOW : reserved_index
                    );
                }
            }
        }
    }
    for (size_t cell = 0; cell < BNU_MEMORY_LAYOUT_WIDTH; ++cell) {
        map[cell] = cell_members[cell] == 0
                        ? '-'
                        : bnu__layout_resolve_symbol(legend, legend_count, combo, combo_count, cell_members[cell]);
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
    bnu__layout_totals_t totals;
    size_t legend_count =
        bnu__build_layout_legend(blocks, shown, processes, process_count, compact_reserved, &totals, legend);
    /* Small and fixed-size - see BNU_MEMORY_COMBO_MAX. */
    bnu__layout_combo_entry_t combo[BNU_MEMORY_COMBO_MAX];
    size_t combo_count = 0;

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
            blocks, shown, processes, process_count, legend, legend_count, compact_reserved, combo, &combo_count,
            region_number, blocks[i].region_start, blocks[i].region_end, blocks[i].region
        );
        previous_start = blocks[i].region_start;
        previous_end = blocks[i].region_end;
    }
    stdio__printf(
        ". free  ? untracked  ! exited owner  - allocator metadata  # too many owners to letter%s\n",
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
    if (compact_reserved && totals.padding_bytes > 0) {
        char bytes_text[16];
        bnu__format_size((uint32_t)totals.padding_bytes, human, bytes_text, sizeof(bytes_text));
        stdio__printf("%c pid %-3s %-15.15s %-8s %8s\n", '+', "-", "(padding)", "reserved", bytes_text);
    }
    /* Combo rows: every map cell where more than one thing - two owners, an
     * owner and some untracked heap noise, free space next to padding,
     * whatever - shares a column gets its own letter (assigned in
     * bnu__layout_resolve_symbol()) plus a breakdown of exactly what's in
     * it, comma-separated. A member that's a specific owner reuses its
     * already-printed total above rather than working out how many of its
     * bytes fall in that one cell; a member that's one of the fixed
     * categories (free/untracked/exited/padding/overflow) reports that
     * category's one backend-wide total instead, for the same reason. */
    for (size_t i = 0; i < combo_count; ++i) {
        stdio__printf("%c ", combo[i].symbol);
        bool first_member = true;
        for (size_t bit = 0; bit < 64; ++bit) {
            if ((combo[i].members & (1ull << bit)) == 0) continue;
            const char *sep = first_member ? "" : ", ";
            char bytes_text[16];
            switch (bit) {
                case BNU_MEMORY_BIT_PADDING:
                    bnu__format_size((uint32_t)totals.padding_bytes, human, bytes_text, sizeof(bytes_text));
                    stdio__printf("%spadding %s", sep, bytes_text);
                    break;
                case BNU_MEMORY_BIT_OVERFLOW: stdio__printf("%sother owners", sep); break;
                case BNU_MEMORY_BIT_FREE:
                    bnu__format_size((uint32_t)totals.free_bytes, human, bytes_text, sizeof(bytes_text));
                    stdio__printf("%sfree %s", sep, bytes_text);
                    break;
                case BNU_MEMORY_BIT_UNTRACKED:
                    bnu__format_size((uint32_t)totals.untracked_bytes, human, bytes_text, sizeof(bytes_text));
                    stdio__printf("%suntracked %s", sep, bytes_text);
                    break;
                case BNU_MEMORY_BIT_EXITED:
                    bnu__format_size((uint32_t)totals.exited_bytes, human, bytes_text, sizeof(bytes_text));
                    stdio__printf("%sexited %s", sep, bytes_text);
                    break;
                default:
                    if (bit < legend_count) {
                        const bruce_process_snapshot_t *process =
                            bnu__layout_process(processes, process_count, legend[bit].owner_id);
                        bnu__format_size((uint32_t)legend[bit].bytes, human, bytes_text, sizeof(bytes_text));
                        stdio__printf(
                            "%s%u %s%s %s", sep, (unsigned)legend[bit].owner_id,
                            process != NULL ? process->name : "<exited>", legend[bit].is_stack ? " (stack)" : "",
                            bytes_text
                        );
                    }
                    break;
            }
            first_member = false;
        }
        stdio__printf("\n");
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
         * there's no PSRAM or it's full too. */
        bool blocks_external = true;
        bruce_memory_layout_block_t *blocks =
            (bruce_memory_layout_block_t *)memory__external_malloc_writable(capacity * sizeof(*blocks));
        if (blocks == NULL) {
            blocks_external = false;
            blocks = memory__malloc(capacity * sizeof(*blocks));
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
