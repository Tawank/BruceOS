#include "bparted.h"

#include <stdint.h>

#include "args.h"
#include "core_sdk/device.h"
#include "core_sdk/memory.h"
#include "core_sdk/partition_manager.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "partition_manager_common.h"

static void bparted__print_reboot_note(void) {
    if (partition_manager__reboot_required()) {
        stdio__printf("Changes are staged; run 'bparted reboot' (or reboot manually) to apply them.\n");
    }
}

static int bparted__list(void) {
    size_t count = 0;
    bruce_result_t result = partition_manager__list(NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    bruce_partition_entry_t *entries = count > 0 ? memory__malloc(count * sizeof(*entries)) : NULL;
    if (count > 0 && entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = partition_manager__list(entries, count, &count);
    if (result != BRUCE_OK) {
        memory__free(entries);
        return result;
    }

    stdio__printf("%-16s %-8s %10s %10s %s\n", "LABEL", "KIND", "OFFSET", "SIZE", "STATUS");
    for (size_t i = 0; i < count; ++i) {
        char offset[32];
        char size[32];
        partition_manager_common__format_size(entries[i].offset, offset, sizeof(offset));
        partition_manager_common__format_size(entries[i].size, size, sizeof(size));
        stdio__printf(
            "%-16s %-8s %10s %10s %s\n", entries[i].label, partition_manager_common__kind_name(entries[i].kind),
            offset, size, entries[i].format_pending ? "pending format" : ""
        );
    }
    memory__free(entries);

    uint64_t free_bytes = 0;
    if (partition_manager__free_space(&free_bytes) == BRUCE_OK) {
        char free_size[32];
        partition_manager_common__format_size(free_bytes, free_size, sizeof(free_size));
        stdio__printf("free: %s\n", free_size);
    }
    bparted__print_reboot_note();
    return 0;
}

static int bparted__create(ArgParser *parser) {
    const char *label = ap_get_arg(parser, "label");
    const char *kind_text = ap_get_arg(parser, "kind");
    const char *size_text = ap_get_arg(parser, "size");

    bruce_partition_kind_t kind;
    uint64_t size_bytes = 0;
    if (!partition_manager_common__parse_kind(kind_text, &kind)) {
        stdio__printf("bparted: kind must be 'swap' or 'littlefs'\n");
        return -1;
    }
    if (!partition_manager_common__parse_size(size_text, &size_bytes)) {
        stdio__printf("bparted: invalid size '%s' (expected e.g. 512K, 2M)\n", size_text);
        return -1;
    }

    bruce_result_t result = partition_manager__stage_create(label, kind, size_bytes);
    if (result != BRUCE_OK) {
        stdio__printf("bparted: create %s failed (%d)\n", label, result);
        return -1;
    }
    result = partition_manager__commit();
    if (result != BRUCE_OK) {
        stdio__printf("bparted: could not save the partition table (%d)\n", result);
        return -1;
    }
    stdio__printf("Staged '%s' (%s).\n", label, kind_text);
    bparted__print_reboot_note();
    return 0;
}

static int bparted__delete(ArgParser *parser) {
    const char *label = ap_get_arg(parser, "label");
    bruce_result_t result = partition_manager__stage_delete(label);
    if (result != BRUCE_OK) {
        stdio__printf("bparted: delete %s failed (%d)\n", label, result);
        return -1;
    }
    result = partition_manager__commit();
    if (result != BRUCE_OK) {
        stdio__printf("bparted: could not save the partition table (%d)\n", result);
        return -1;
    }
    stdio__printf("Staged deletion of '%s'.\n", label);
    bparted__print_reboot_note();
    return 0;
}

static int bparted__format(ArgParser *parser) {
    const char *label = ap_get_arg(parser, "label");
    bruce_result_t result = partition_manager__stage_format(label);
    if (result != BRUCE_OK) {
        stdio__printf("bparted: format %s failed (%d)\n", label, result);
        return -1;
    }
    result = partition_manager__commit();
    if (result != BRUCE_OK) {
        stdio__printf("bparted: could not save the partition table (%d)\n", result);
        return -1;
    }
    stdio__printf("Staged format of '%s'.\n", label);
    bparted__print_reboot_note();
    return 0;
}

static int bparted__reboot(void) {
    stdio__printf("Rebooting...\n");
    return device__restart(500) == BRUCE_OK ? 0 : -1;
}

int bparted_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return -1;
    ap_set_helptext(root, "Manage the user flash area beyond the static partition table.");

    ArgParser *list = ap_new_cmd(root, "list");
    ArgParser *create = ap_new_cmd(root, "create");
    ArgParser *delete_cmd = ap_new_cmd(root, "delete rm");
    ArgParser *format_cmd = ap_new_cmd(root, "format");
    ArgParser *reboot = ap_new_cmd(root, "reboot");

    ArgParser *commands[] = {list, create, delete_cmd, format_cmd, reboot};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (commands[i] == NULL) {
            ap_free(root);
            return -1;
        }
    }

    ap_set_helptext(list, "List partitions in the user area, free space, and pending changes.");
    ap_set_helptext(create, "Create ('mkpart'-style) a partition: create <label> <swap|littlefs> <size>.");
    ap_add_required_arg(create, "label", "Partition label (reserved: 'swap', 'littlefs')");
    ap_add_required_arg(create, "kind", "'swap' or 'littlefs'");
    ap_add_required_arg(create, "size", "Size, e.g. 512K, 2M");
    ap_set_helptext(delete_cmd, "Delete a partition by label (the root 'littlefs' cannot be deleted).");
    ap_add_required_arg(delete_cmd, "label", "Partition label");
    ap_set_helptext(format_cmd, "Erase and reformat a partition by label, keeping its size and position.");
    ap_add_required_arg(format_cmd, "label", "Partition label");
    ap_set_helptext(reboot, "Reboot to apply staged partition changes.");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        return status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? 0 : -1;
    }

    int result = -1;
    ArgParser *command = ap_get_cmd_parser(root);
    if (command == NULL || command == list) result = bparted__list();
    else if (command == create) result = bparted__create(create);
    else if (command == delete_cmd) result = bparted__delete(delete_cmd);
    else if (command == format_cmd) result = bparted__format(format_cmd);
    else if (command == reboot) result = bparted__reboot();

    ap_free(root);
    return result;
}
