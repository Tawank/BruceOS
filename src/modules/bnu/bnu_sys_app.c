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

#define BNU_MEMORY_LAYOUT_WIDTH 48

static const bruce_process_snapshot_t *
bnu__layout_process(const bruce_process_snapshot_t *processes, size_t count, bruce_process_id_t id) {
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].id == id) return &processes[i];
    }
    return NULL;
}

/* One map/legend letter per (owner, used-or-reserved) pair that actually
 * appears in a backend's blocks - built fresh per `free -m` call from
 * whichever processes and reservations are live right now, rather than
 * reserving a fixed letter per system process up front. That's what lets the
 * legend list only processes actually visible in the map (see
 * bnu__build_layout_legend()) instead of every process on the system. Each
 * entry also totals the bytes it stands for, so a map cell collapsed to '#'
 * (more distinct owners than the map has columns for) is never the only way
 * to find out who's actually there and how much they hold - the legend
 * always has the exact number. */
#define BNU_MEMORY_LEGEND_MAX 52

typedef struct {
    char symbol;
    bruce_process_id_t owner_id;
    /* false: this letter marks bytes actually holding that owner's data.
     * true: bytes reserved for that owner but not currently holding any -
     * either the rounded-up tail of one of its own allocations (block->size
     * > block->requested_size) or a free slot run on a swap slab page it
     * exclusively holds (see memory_external__allocate_slot_locked()'s doc
     * comment - such a page's spare slots aren't available to anyone else,
     * so "free" alone would be misleading). */
    bool reserved;
    size_t bytes;
} bnu__layout_legend_entry_t;

static char bnu__layout_legend_symbol(
    const bnu__layout_legend_entry_t *legend, size_t legend_count, bruce_process_id_t owner_id, bool reserved
) {
    for (size_t i = 0; i < legend_count; ++i) {
        if (legend[i].owner_id == owner_id && legend[i].reserved == reserved) return legend[i].symbol;
    }
    /* Ran out of the 52 available letters - same overflow behaviour the
     * previous flat per-process assignment used. */
    return '*';
}

static void bnu__layout_legend_add_bytes(
    bnu__layout_legend_entry_t *legend, size_t *legend_count, bruce_process_id_t owner_id, bool reserved,
    size_t bytes
) {
    for (size_t i = 0; i < *legend_count; ++i) {
        if (legend[i].owner_id == owner_id && legend[i].reserved == reserved) {
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
        .bytes = bytes,
    };
}

/* Scans every block this backend reported (not just one region) so the same
 * owner gets the same letter in every region's map, and assigns letters in
 * address order (blocks are already sorted by the time this runs) - the
 * closest thing to "in the order you'd read the maps" without tracking each
 * region separately. Blocks whose disposition wouldn't get a dedicated
 * letter anyway (untracked - '?', or a used block whose owner has since
 * exited - '!') are skipped, exactly mirroring what
 * bnu__print_layout_region() actually paints for them. */
static size_t bnu__build_layout_legend(
    const bruce_memory_layout_block_t *blocks, size_t count, const bruce_process_snapshot_t *processes,
    size_t process_count, bnu__layout_legend_entry_t *legend
) {
    size_t legend_count = 0;
    for (size_t i = 0; i < count; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
        if (block->owner_id == BRUCE_PROCESS_ID_INVALID) continue;
        if (block->used) {
            if (!block->tracked) continue;
            if (bnu__layout_process(processes, process_count, block->owner_id) == NULL) continue;
            bnu__layout_legend_add_bytes(legend, &legend_count, block->owner_id, false, block->requested_size);
            if (block->size > block->requested_size) {
                bnu__layout_legend_add_bytes(
                    legend, &legend_count, block->owner_id, true, block->size - block->requested_size
                );
            }
        } else {
            bnu__layout_legend_add_bytes(legend, &legend_count, block->owner_id, true, block->size);
        }
    }
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

static void bnu__layout_paint(char *map, size_t first, size_t last, char symbol) {
    for (size_t cell = first; cell < last; ++cell) {
        if (map[cell] == '-' || map[cell] == symbol) map[cell] = symbol;
        else map[cell] = '#';
    }
}

static void bnu__print_layout_region(
    const bruce_memory_layout_block_t *blocks, size_t count,
    const bruce_process_snapshot_t *processes, size_t process_count,
    const bnu__layout_legend_entry_t *legend, size_t legend_count,
    uintptr_t region_start, uintptr_t region_end, bruce_memory_region_t region
) {
    if (region_end <= region_start) return;
    size_t span = region_end - region_start;
    char map[BNU_MEMORY_LAYOUT_WIDTH + 1];
    memset(map, '-', BNU_MEMORY_LAYOUT_WIDTH);
    map[BNU_MEMORY_LAYOUT_WIDTH] = '\0';
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
            /* Free: '.' when nobody's holding it, otherwise the reserved
             * letter for whoever is (see bnu__build_layout_legend()). */
            char symbol = block->owner_id == BRUCE_PROCESS_ID_INVALID
                              ? '.'
                              : bnu__layout_legend_symbol(legend, legend_count, block->owner_id, true);
            bnu__layout_paint(map, first, last, symbol);
        } else if (!block->tracked) {
            bnu__layout_paint(map, first, last, '?');
        } else if (bnu__layout_process(processes, process_count, block->owner_id) == NULL) {
            bnu__layout_paint(map, first, last, '!');
        } else {
            /* Split the block's own span at the boundary between bytes it
             * actually asked for (block->requested_size) and the rounded-up
             * remainder its allocator reserved alongside them
             * (block->size - block->requested_size, if any) - each half
             * gets that owner's own used/reserved letter. */
            size_t requested_end = relative + block->requested_size;
            if (requested_end > end) requested_end = end;
            size_t used_last = (requested_end * BNU_MEMORY_LAYOUT_WIDTH + span - 1) / span;
            if (used_last > last) used_last = last;
            if (used_last < first) used_last = first;
            char used_symbol = bnu__layout_legend_symbol(legend, legend_count, block->owner_id, false);
            bnu__layout_paint(map, first, used_last, used_symbol);
            if (last > used_last) {
                char reserved_symbol = bnu__layout_legend_symbol(legend, legend_count, block->owner_id, true);
                bnu__layout_paint(map, used_last, last, reserved_symbol);
            }
        }
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
        "region %-12s 0x%08lx-0x%08lx (%s/%s) free: %s lrgst: %s\n[%s]\n",
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

    bnu__layout_legend_entry_t legend[BNU_MEMORY_LEGEND_MAX];
    size_t legend_count = bnu__build_layout_legend(blocks, shown, processes, process_count, legend);

    char total_text[16];
    bnu__format_size((uint32_t)total, true, total_text, sizeof(total_text));
    stdio__printf("\n%s layout (%s, physical regions)\n", name, total_text);
    uintptr_t previous_start = UINTPTR_MAX;
    uintptr_t previous_end = UINTPTR_MAX;
    for (size_t i = 0; i < shown; ++i) {
        if (blocks[i].region_start == previous_start && blocks[i].region_end == previous_end) continue;
        bnu__print_layout_region(
            blocks, shown, processes, process_count, legend, legend_count,
            blocks[i].region_start, blocks[i].region_end, blocks[i].region
        );
        previous_start = blocks[i].region_start;
        previous_end = blocks[i].region_end;
    }
    stdio__printf(". free  ? untracked  # multiple owners (see legend)  ! exited owner  - allocator metadata\n");
    for (size_t i = 0; i < legend_count; ++i) {
        const bruce_process_snapshot_t *process = bnu__layout_process(processes, process_count, legend[i].owner_id);
        char bytes_text[16];
        bnu__format_size((uint32_t)legend[i].bytes, human, bytes_text, sizeof(bytes_text));
        stdio__printf(
            "%c pid %-3u %-15.15s %-8s %8s\n", legend[i].symbol, (unsigned)legend[i].owner_id,
            process != NULL ? process->name : "<exited>", legend[i].reserved ? "reserved" : "used", bytes_text
        );
    }

    stdio__printf(
        "%-10s %-5s %-4s %-15s %8s %8s\n", "address", "state", "pid", "owner", "request", "block"
    );
    for (size_t i = 0; i < shown; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
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
            "0x%08lx %-5s %-4u %-15.15s %8s %8s%s\n",
            (unsigned long)block->address, block->used ? "used" : "free",
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
        bruce_process_snapshot_t processes[16];
        size_t process_count = 0;
        result = process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
        if (result != BRUCE_OK) {
            stdio__printf("free: -m layout unavailable: %s\n", result__to_string(result));
            if (blocks_external) memory__external_free(blocks);
            else memory__free(blocks);
            return result;
        }
        if (process_count > sizeof(processes) / sizeof(processes[0])) {
            process_count = sizeof(processes) / sizeof(processes[0]);
        }
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

    bruce_process_snapshot_t processes[16];
    size_t process_count = 0;
    bruce_result_t result =
        process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
    if (result != BRUCE_OK) return result;
    result = runtime__delay(250);
    if (result != BRUCE_OK) return result;
    result = process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
    if (result != BRUCE_OK) return result;

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
