#include "bnu_app.h"
#include "bnu_internal.h"

#include <stdio.h>

#include "args.h"
#include "core_sdk/disk.h"
#include "core_sdk/format.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

/* Block device commands: lsblk, mount, unmount. */

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
