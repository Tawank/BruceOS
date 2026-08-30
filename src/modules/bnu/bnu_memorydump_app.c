#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/runtime.h"

#include "args.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

#define BNU_MEMORYDUMP_BINARY_CHUNK 64u
#define BNU_MEMORYDUMP_DEFAULT_COLUMNS 16u
#define BNU_MEMORYDUMP_MAX_COLUMNS 32u
#define BNU_MEMORYDUMP_YIELD_BYTES 512u

static bruce_result_t bnu__memorydump_yield_output(size_t bytes, size_t *pending) {
    *pending += bytes;
    if (*pending < BNU_MEMORYDUMP_YIELD_BYTES) return BRUCE_OK;

    /* Serial writes can remain continuously ready while still monopolizing CPU0.
     * Blocking for one tick lets the idle task run and feed its watchdog. */
    *pending = 0;
    return runtime__delay(1);
}

static bool bnu__memorydump_parse_u64(const char *text, uint64_t *out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') return false;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (end == text || *end != '\0') return false;
    *out = (uint64_t)value;
    return true;
}

static bool bnu__memorydump_backend(const char *text, bruce_memory_backend_t *out) {
    if (strcmp(text, "int") == 0 || strcmp(text, "internal") == 0) {
        *out = BRUCE_MEMORY_BACKEND_INTERNAL;
    } else if (strcmp(text, "psram") == 0) {
        *out = BRUCE_MEMORY_BACKEND_PSRAM;
    } else if (strcmp(text, "swap") == 0) {
        *out = BRUCE_MEMORY_BACKEND_SWAP;
    } else {
        return false;
    }
    return true;
}

static int bnu__memorydump_compare_address(const void *left, const void *right) {
    const bruce_memory_layout_block_t *a = left;
    const bruce_memory_layout_block_t *b = right;
    if (a->address < b->address) return -1;
    if (a->address > b->address) return 1;
    return 0;
}

static const bruce_process_snapshot_t *bnu__memorydump_find_process(
    const bruce_process_snapshot_t *processes, size_t count, bruce_process_id_t id
) {
    for (size_t i = 0; i < count; ++i) {
        if (processes[i].id == id) return &processes[i];
    }
    return NULL;
}

static bruce_result_t bnu__memorydump_range(
    bruce_memory_backend_t backend, uintptr_t address, size_t length,
    bool hex, size_t columns, size_t group, size_t *pending_yield
) {
    bruce_result_t result = memory__read(backend, address, NULL, length);
    if (result != BRUCE_OK) return result;
    uint8_t buffer[BNU_MEMORYDUMP_BINARY_CHUNK];
    size_t offset = 0;
    while (offset < length) {
        size_t chunk_limit = hex ? columns : sizeof(buffer);
        size_t chunk = length - offset > chunk_limit ? chunk_limit : length - offset;
        result = memory__read(backend, address + offset, buffer, chunk);
        if (result != BRUCE_OK) break;
        if (hex) bnu__xxd_print_line(buffer, chunk, address + offset, columns, group, false);
        else result = stdio__write(buffer, chunk);
        if (result != BRUCE_OK) break;
        offset += chunk;
        result = bnu__memorydump_yield_output(chunk, pending_yield);
        if (result != BRUCE_OK) break;
    }
    if (result != BRUCE_OK && hex) {
        stdio__printf(
            "memorydump: stopped at 0x%08lx: %s\n",
            (unsigned long)(address + offset), result__to_string(result)
        );
    }
    return result;
}

static bruce_result_t bnu__memorydump_all(
    bruce_memory_backend_t backend, bool hex, bool owners, size_t columns, size_t group
) {
    size_t required = 0;
    bruce_result_t result = memory__get_layout(backend, NULL, 0, &required);
    if (result != BRUCE_OK) return result;
    if (required > SIZE_MAX - 4 || required + 4 > SIZE_MAX / sizeof(bruce_memory_layout_block_t)) {
        return BRUCE_ERR_NO_MEMORY;
    }
    size_t capacity = required + 4;
    bruce_memory_layout_block_t *blocks = memory__malloc(capacity * sizeof(*blocks));
    if (blocks == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t count = 0;
    result = memory__get_layout(backend, blocks, capacity, &count);
    if (result != BRUCE_OK || count > capacity) {
        memory__free(blocks);
        return result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT;
    }
    qsort(blocks, count, sizeof(*blocks), bnu__memorydump_compare_address);

    bruce_process_snapshot_t processes[16];
    size_t pending_yield = 0;
    size_t process_count = 0;
    if (owners) {
        result = process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
        if (result != BRUCE_OK) {
            memory__free(blocks);
            return result;
        }
        if (process_count > sizeof(processes) / sizeof(processes[0])) {
            process_count = sizeof(processes) / sizeof(processes[0]);
        }
    }

    for (size_t i = 0; i < count && result == BRUCE_OK; ++i) {
        const bruce_memory_layout_block_t *block = &blocks[i];
        if (backend == BRUCE_MEMORY_BACKEND_SWAP && !block->used) continue;
        size_t length = backend == BRUCE_MEMORY_BACKEND_SWAP ? block->requested_size : block->size;
        if (length == 0) continue;
        if (owners) {
            const bruce_process_snapshot_t *process = bnu__memorydump_find_process(
                processes, process_count, block->owner_id
            );
            const char *owner = !block->used ? "free"
                                : !block->tracked ? "untracked"
                                : process != NULL ? process->name : "<exited>";
            stdio__printf(
                "# block 0x%08lx size=%u dump=%u state=%s owner=%s pid=%u requested=%u%s\n",
                (unsigned long)block->address, (unsigned)block->size, (unsigned)length,
                block->used ? "used" : "free", owner, (unsigned)block->owner_id,
                (unsigned)block->requested_size, block->executable ? " xip" : ""
            );
        }
        result = bnu__memorydump_range(
            backend, block->address, length, hex, columns, group, &pending_yield
        );
    }
    memory__free(blocks);
    return result;
}

int bnu_memorydump_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser(
        "Dump a validated internal-RAM, PSRAM, or active-swap range as binary."
    );
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "x");
    ap_set_opt_help(parser, "x", "Print xxd-style hexadecimal instead of binary");
    ap_add_flag(parser, "a");
    ap_set_opt_help(parser, "a", "Dump every heap block (active objects only for swap)");
    ap_add_flag(parser, "o");
    ap_set_opt_help(parser, "o", "Show each block's owner header (requires -a -x)");
    ap_add_str_opt(parser, "c", NULL);
    ap_set_opt_help(parser, "c", "Hex mode bytes per line (1..32; default 16)");
    ap_add_str_opt(parser, "g", NULL);
    ap_set_opt_help(parser, "g", "Hex mode group size (1, 2, 4, or 8; default 2)");
    ap_add_required_arg(parser, "backend", "int, internal, psram, or swap");
    ap_add_optional_arg(parser, "address", "RAM address, or byte offset for swap (omit with -a)");
    ap_add_optional_arg(parser, "length", "Bytes to dump (default: remainder of containing block/object)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bruce_memory_backend_t backend = BRUCE_MEMORY_BACKEND_INVALID;
    uint64_t address64 = 0;
    uint64_t length64 = 0;
    uint64_t parsed = 0;
    size_t columns = BNU_MEMORYDUMP_DEFAULT_COLUMNS;
    size_t group = 2;
    bool hex = ap_found(parser, "x");
    bool all = ap_found(parser, "a");
    bool owners = ap_found(parser, "o");
    const char *address_arg = ap_get_arg(parser, "address");
    const char *length_arg = ap_get_arg(parser, "length");
    bool valid = bnu__memorydump_backend(ap_get_arg(parser, "backend"), &backend) &&
                 ((all && address_arg == NULL && length_arg == NULL) ||
                  (!all && bnu__memorydump_parse_u64(address_arg, &address64) &&
                   address64 <= UINTPTR_MAX)) &&
                 (!owners || (all && hex));
    if (valid && !all && length_arg != NULL) {
        valid = bnu__memorydump_parse_u64(length_arg, &length64) && length64 > 0 &&
                length64 <= SIZE_MAX && address64 <= UINTPTR_MAX - length64;
    }
    if (valid && ap_found(parser, "c")) {
        valid = hex && bnu__memorydump_parse_u64(ap_get_str_value(parser, "c"), &parsed) &&
                parsed >= 1 && parsed <= BNU_MEMORYDUMP_MAX_COLUMNS;
        if (valid) columns = (size_t)parsed;
    }
    if (valid && ap_found(parser, "g")) {
        valid = hex && bnu__memorydump_parse_u64(ap_get_str_value(parser, "g"), &parsed) &&
                (parsed == 1 || parsed == 2 || parsed == 4 || parsed == 8);
        if (valid) group = (size_t)parsed;
    }
    if (!valid) {
        stdio__printf("memorydump: invalid backend, range, or option combination\n");
        ap_free(parser);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    if (all) {
        bruce_result_t result = bnu__memorydump_all(backend, hex, owners, columns, group);
        ap_free(parser);
        return result;
    }

    uintptr_t address = (uintptr_t)address64;
    size_t length = 0;
    size_t pending_yield = 0;
    bruce_result_t result = length_arg == NULL
                                ? memory__readable_size(backend, address, &length)
                                : BRUCE_OK;
    if (length_arg != NULL) length = (size_t)length64;
    if (result != BRUCE_OK) {
        stdio__printf("memorydump: address is not readable: %s\n", result__to_string(result));
        ap_free(parser);
        return result;
    }
    result = bnu__memorydump_range(
        backend, address, length, hex, columns, group, &pending_yield
    );
    ap_free(parser);
    return result;
}
