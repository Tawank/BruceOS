#include "bparted_cli.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "args.h"
#include "core_sdk/device.h"
#include "core_sdk/partition_manager.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

#include "bparted_common.h"

/* The terminal half of bparted, and a peer of the GUI rather than a layer
 * under it: both call core_sdk/partition_manager.h directly.
 *
 * Every verb that changes something (create/delete/format) only edits the
 * layout the next boot will use; nothing is written until 'apply', and
 * nothing on flash is erased or reformatted until the reboot after that.
 * '--apply' folds the save into the same command, which is what a one-shot
 * recovery session over a serial line wants (see app_main()'s
 * recover_missing_partitions() - a device with no usable storage is fixed
 * from here). */

static int bparted_cli__fail(const char *action, bruce_result_t result) {
    stdio__printf("bparted: %s failed: %s (%d)\n", action, bparted_common__error_text(result), (int)result);
    return -1;
}

static void bparted_cli__print_layout(
    const char *heading, const bruce_partition_entry_t *entries, size_t count
) {
    stdio__printf("%s\n", heading);
    stdio__printf("  %-16s %-8s %10s %10s %-5s %s\n", "LABEL", "KIND", "OFFSET", "SIZE", "MOUNT", "CHANGE");
    for (size_t i = 0; i < count; ++i) {
        char offset[16];
        char size[16];
        bparted_common__format_size(entries[i].offset, offset, sizeof(offset));
        bparted_common__format_size(entries[i].size, size, sizeof(size));
        stdio__printf(
            "  %-16s %-8s %10s %10s %-5s %s\n", entries[i].label,
            bparted_common__kind_name(entries[i].kind), offset, size, entries[i].is_root ? "/" : "-",
            bparted_common__state_name(entries[i].state)
        );
    }
}

static void bparted_cli__print_status(const bruce_partition_status_t *status) {
    char total[16];
    char used[16];
    char unallocated[16];
    char max_new[16];
    bparted_common__format_size(status->total_bytes, total, sizeof(total));
    bparted_common__format_size(status->used_bytes, used, sizeof(used));
    bparted_common__format_size(status->unallocated_bytes, unallocated, sizeof(unallocated));
    bparted_common__format_size(status->max_new_size, max_new, sizeof(max_new));
    stdio__printf(
        "user area %s: %s used, %s unallocated (largest new partition %s)\n", total, used, unallocated, max_new
    );

    if (status->has_pending_changes) {
        stdio__printf("Not applied yet - run 'bparted apply' to save, 'bparted cancel' to throw away.\n");
    } else if (status->reboot_required) {
        stdio__printf("Saved but not in effect - run 'bparted reboot' to boot into it.\n");
    } else {
        stdio__printf("The next boot uses the layout running now.\n");
    }
}

/* Both layouts plus the accounting, which is what "what state is this
 * device in" needs and is cheap enough not to need a narrower default. */
static int bparted_cli__list(void) {
    bruce_partition_entry_t current[BPARTED_LAYOUT_MAX];
    size_t current_count = 0;
    bruce_result_t result = bparted_common__clamp_list(
        partition_manager__list_current(current, BPARTED_LAYOUT_MAX, &current_count), &current_count,
        BPARTED_LAYOUT_MAX
    );
    if (result != BRUCE_OK) return bparted_cli__fail("list", result);

    bruce_partition_entry_t planned[BPARTED_PLANNED_MAX];
    size_t planned_count = 0;
    result = bparted_common__clamp_list(
        partition_manager__list_planned(planned, BPARTED_PLANNED_MAX, &planned_count), &planned_count,
        BPARTED_PLANNED_MAX
    );
    if (result != BRUCE_OK) return bparted_cli__fail("list", result);

    bruce_partition_status_t status;
    result = partition_manager__status(&status);
    if (result != BRUCE_OK) return bparted_cli__fail("list", result);

    bparted_cli__print_layout("Running now:", current, current_count);
    stdio__printf("\n");
    bparted_cli__print_layout("After next boot:", planned, planned_count);
    stdio__printf("\n");
    bparted_cli__print_status(&status);
    return 0;
}

static int bparted_cli__status(void) {
    bruce_partition_status_t status;
    bruce_result_t result = partition_manager__status(&status);
    if (result != BRUCE_OK) return bparted_cli__fail("status", result);
    bparted_cli__print_status(&status);
    return 0;
}

static int bparted_cli__commit(void) {
    bruce_result_t result = partition_manager__commit();
    if (result != BRUCE_OK) return bparted_cli__fail("apply", result);
    stdio__printf("Saved. It takes effect on the next boot - run 'bparted reboot'.\n");
    return 0;
}

/* Reports what a create/delete/format just changed, then either saves it
 * right away (--apply) or says how to. */
static int bparted_cli__staged(const char *message, ArgParser *parser) {
    stdio__printf("%s\n", message);
    if (ap_found(parser, "apply")) return bparted_cli__commit();
    stdio__printf("Run 'bparted apply' to save it, 'bparted cancel' to throw it away.\n");
    return 0;
}

static int bparted_cli__create(ArgParser *parser) {
    const char *label = ap_get_arg(parser, "label");
    const char *kind_text = ap_get_arg(parser, "kind");
    const char *size_text = ap_get_arg(parser, "size");

    bruce_partition_kind_t kind;
    if (!bparted_common__parse_kind(kind_text, &kind)) {
        stdio__printf("bparted: kind must be 'swap' or 'littlefs'\n");
        return -1;
    }
    uint64_t size_bytes = 0;
    if (!bparted_common__parse_size(size_text, &size_bytes)) {
        stdio__printf("bparted: invalid size '%s' (expected e.g. 512K, 2M)\n", size_text);
        return -1;
    }

    bruce_result_t result = partition_manager__stage_create(label, kind, size_bytes);
    if (result != BRUCE_OK) return bparted_cli__fail("create", result);

    char message[80];
    snprintf(message, sizeof(message), "Added '%s' to the next-boot layout.", label);
    return bparted_cli__staged(message, parser);
}

static int bparted_cli__delete(ArgParser *parser) {
    const char *label = ap_get_arg(parser, "label");
    bruce_result_t result = partition_manager__stage_delete(label);
    if (result != BRUCE_OK) return bparted_cli__fail("delete", result);

    char message[80];
    snprintf(message, sizeof(message), "Removed '%s' from the next-boot layout.", label);
    return bparted_cli__staged(message, parser);
}

static int bparted_cli__format(ArgParser *parser) {
    const char *label = ap_get_arg(parser, "label");
    bruce_result_t result = partition_manager__stage_format(label);
    if (result != BRUCE_OK) return bparted_cli__fail("format", result);

    char message[80];
    snprintf(message, sizeof(message), "Marked '%s' to be erased and reformatted.", label);
    return bparted_cli__staged(message, parser);
}

static int bparted_cli__apply(void) {
    bruce_partition_status_t status;
    bruce_result_t result = partition_manager__status(&status);
    if (result != BRUCE_OK) return bparted_cli__fail("apply", result);
    if (!status.has_pending_changes) {
        stdio__printf("bparted: nothing to apply\n");
        return 0;
    }
    return bparted_cli__commit();
}

static int bparted_cli__cancel(void) {
    bruce_partition_status_t status;
    bruce_result_t result = partition_manager__status(&status);
    if (result != BRUCE_OK) return bparted_cli__fail("cancel", result);
    if (!status.has_pending_changes) {
        stdio__printf("bparted: nothing to cancel\n");
        return 0;
    }
    result = partition_manager__discard();
    if (result != BRUCE_OK) return bparted_cli__fail("cancel", result);
    stdio__printf("Threw away every change that had not been applied.\n");
    return 0;
}

static int bparted_cli__reboot(void) {
    stdio__printf("Rebooting...\n");
    return device__restart(500) == BRUCE_OK ? 0 : -1;
}

/* Registers --apply/-a on a verb that stages something. */
static void bparted_cli__add_apply_flag(ArgParser *parser) {
    ap_add_flag(parser, "apply a");
    ap_set_opt_help(parser, "apply", "Save the new layout immediately, instead of waiting for 'bparted apply'");
}

int bparted_cli__main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return -1;
    ap_set_helptext(
        root, "Manage the user flash area: the space left over after the built-in partitions. Changes are "
              "collected into the layout the next boot will use, and only touch flash once applied."
    );

    ArgParser *list = ap_new_cmd(root, "list ls");
    ArgParser *status = ap_new_cmd(root, "status");
    ArgParser *create = ap_new_cmd(root, "create mkpart");
    ArgParser *delete_cmd = ap_new_cmd(root, "delete rm");
    ArgParser *format_cmd = ap_new_cmd(root, "format");
    ArgParser *apply = ap_new_cmd(root, "apply");
    ArgParser *cancel = ap_new_cmd(root, "cancel");
    ArgParser *reboot = ap_new_cmd(root, "reboot");

    ArgParser *commands[] = {list, status, create, delete_cmd, format_cmd, apply, cancel, reboot};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (commands[i] == NULL) {
            ap_free(root);
            return -1;
        }
    }

    ap_set_helptext(list, "Show the layout running now, the one the next boot will use, and free space.");
    ap_set_helptext(status, "Show free space and whether anything is waiting to be applied or rebooted into.");

    ap_set_helptext(create, "Add a partition: create <label> <swap|littlefs> <size>.");
    ap_add_required_arg(create, "label", "Partition label ('swap' is reserved for a swap partition)");
    ap_add_required_arg(create, "kind", "'swap' or 'littlefs'");
    ap_add_required_arg(create, "size", "Size, e.g. 512K, 2M");
    bparted_cli__add_apply_flag(create);

    ap_set_helptext(delete_cmd, "Remove a partition by label (the root partition can only be formatted).");
    ap_add_required_arg(delete_cmd, "label", "Partition label");
    bparted_cli__add_apply_flag(delete_cmd);

    ap_set_helptext(format_cmd, "Erase and reformat a partition, keeping its size and position.");
    ap_add_required_arg(format_cmd, "label", "Partition label");
    bparted_cli__add_apply_flag(format_cmd);

    ap_set_helptext(apply, "Save every change made so far; it takes effect on the next boot.");
    ap_set_helptext(cancel, "Throw away every change that has not been applied.");
    ap_set_helptext(reboot, "Reboot, so a saved layout takes effect.");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t parse_status = ap_get_status(root);
        ap_free(root);
        return parse_status == AP_STATUS_HELP || parse_status == AP_STATUS_VERSION ? 0 : -1;
    }

    int result = -1;
    ArgParser *command = ap_get_cmd_parser(root);
    if (command == NULL || command == list) result = bparted_cli__list();
    else if (command == status) result = bparted_cli__status();
    else if (command == create) result = bparted_cli__create(create);
    else if (command == delete_cmd) result = bparted_cli__delete(delete_cmd);
    else if (command == format_cmd) result = bparted_cli__format(format_cmd);
    else if (command == apply) result = bparted_cli__apply();
    else if (command == cancel) result = bparted_cli__cancel();
    else if (command == reboot) result = bparted_cli__reboot();

    ap_free(root);
    return result;
}
