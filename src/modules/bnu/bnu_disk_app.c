#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdio.h>

#include "args.h"
#include "core_sdk/disk.h"
#include "core_sdk/format.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/* Block device commands: lsblk, mount, unmount, df. */

int bnu_lsblk_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("List block devices and partitions.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);

    size_t count = 0;
    bruce_result_t result = disk__list(NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    bruce_disk_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = disk__list(entries, count, &count);
    if (result != BRUCE_OK) {
        memory__free(entries);
        return result;
    }

    stdio__printf("%-18s %2s %6s %2s %-4s %s\n", "NAME", "RM", "SIZE", "RO", "TYPE", "MOUNTPOINTS");
    for (size_t i = 0; i < count; ++i) {
        char size[32];
        char name[BRUCE_DISK_NAME_MAX + 3];
        format__bytes_human(entries[i].size, size, sizeof(size));
        snprintf(
            name,
            sizeof(name),
            "%s%s",
            entries[i].type == BRUCE_DISK_TYPE_PARTITION
                ? (i + 1 == count || entries[i + 1].parent[0] == '\0' ? "`-" : "|-")
                : "",
            entries[i].name
        );
        stdio__printf(
            "%-18s %2u %6s %2u %-4s %s\n",
            name,
            entries[i].removable ? 1u : 0u,
            size,
            entries[i].read_only ? 1u : 0u,
            entries[i].type == BRUCE_DISK_TYPE_PARTITION ? "part" : "disk",
            entries[i].mount_point
        );
    }
    memory__free(entries);
    return BRUCE_OK;
}

static int bnu__list_mounts(void) {
    size_t count = 0;
    bruce_result_t result = disk__list(NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    bruce_disk_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = disk__list(entries, count, &count);
    if (result == BRUCE_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].mount_point[0] == '\0') continue;
            stdio__printf(
                "%s on %s type %s (rw)\n",
                entries[i].name,
                entries[i].mount_point,
                entries[i].type == BRUCE_DISK_TYPE_PARTITION ? "littlefs" : "fat"
            );
        }
    }
    memory__free(entries);
    return result;
}

int bnu_mount_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("List mounted filesystems or mount a block device.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_optional_arg(parser, "device", "Block device name (currently sd0)");
    ap_add_optional_arg(parser, "mount-point", "Mount point (defaults to /sdcard)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *device = ap_get_arg(parser, "device");
    const char *mount_point = ap_get_arg(parser, "mount-point");
    if (device == NULL) {
        ap_free(parser);
        return bnu__list_mounts();
    }
    bruce_result_t result = disk__mount(device, mount_point != NULL ? mount_point : "/sdcard");
    if (result != BRUCE_OK) stdio__printf("mount: %s: error %d\n", device, result);
    ap_free(parser);
    return result;
}

int bnu_unmount_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Unmount a block device or mount point.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "target", "Block device name or mount point");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *target = ap_get_arg(parser, "target");
    bruce_result_t result = disk__unmount(target);
    if (result != BRUCE_OK) stdio__printf("unmount: %s: error %d\n", target, result);
    ap_free(parser);
    return result;
}

/* One df row: storage__get_usage() takes any path on the volume, not just
 * its mount point, but every caller below passes a mount point straight out
 * of disk__list() (or the user's own --path, printed back as given rather
 * than resolved to its owning mount point - a minor cosmetic gap next to
 * real df). */
static bruce_result_t bnu__df_print_row(const char *mount_point, bool human) {
    size_t total = 0, used = 0;
    bruce_result_t result = storage__get_usage(mount_point, &total, &used);
    if (result != BRUCE_OK) {
        stdio__printf("df: %s: error %d\n", mount_point, result);
        return result;
    }
    size_t avail = total > used ? total - used : 0;
    /* used*100 alone would overflow a 32-bit size_t well before `used`
     * itself gets anywhere near a real SD card's capacity (>~42MB used is
     * already enough) - widen to uint64_t for the multiply. */
    unsigned percent = total > 0 ? (unsigned)(((uint64_t)used * 100 + total / 2) / total) : 0;

    char total_text[32], used_text[32], avail_text[32];
    if (human) {
        format__bytes_human((uint64_t)total, total_text, sizeof(total_text));
        format__bytes_human((uint64_t)used, used_text, sizeof(used_text));
        format__bytes_human((uint64_t)avail, avail_text, sizeof(avail_text));
    } else {
        snprintf(total_text, sizeof(total_text), "%llu", (unsigned long long)total);
        snprintf(used_text, sizeof(used_text), "%llu", (unsigned long long)used);
        snprintf(avail_text, sizeof(avail_text), "%llu", (unsigned long long)avail);
    }
    stdio__printf("%-10s %8s %8s %8s %4u%%\n", mount_point, total_text, used_text, avail_text, percent);
    return BRUCE_OK;
}

int bnu_df_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show mounted filesystems' space usage.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_flag(parser, "h");
    ap_set_opt_help(parser, "h", "Show sizes in human-readable units (e.g. 8.2K, 1.3M)");
    ap_add_optional_arg(parser, "path", "Show only the filesystem containing this path");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    bool human = ap_found(parser, "h");
    const char *path_arg = ap_get_arg(parser, "path");
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = path_arg == NULL || bnu__resolve_path(path_arg, path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;

    stdio__printf("%-10s %8s %8s %8s %5s\n", "mount", "total", "used", "avail", "use%");
    if (path_arg != NULL) return bnu__df_print_row(path, human);

    size_t count = 0;
    bruce_result_t result = disk__list(NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    bruce_disk_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = disk__list(entries, count, &count);
    if (result != BRUCE_OK) {
        memory__free(entries);
        return result;
    }
    bruce_result_t first_error = BRUCE_OK;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].mount_point[0] == '\0') continue;
        bruce_result_t row_result = bnu__df_print_row(entries[i].mount_point, human);
        if (row_result != BRUCE_OK && first_error == BRUCE_OK) first_error = row_result;
    }
    memory__free(entries);
    return first_error;
}
