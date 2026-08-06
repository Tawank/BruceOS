#include "partition_manager_gui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/partition_manager.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "partition_manager_common.h"

#define PARTITION_MANAGER_GUI_MAX_ENTRIES 8

/* Every mutation drives the "bparted" CLI command (modules/partition_manager
 * /bparted.c) via app_runner__run_command(), the same shell-command-from-GUI
 * mechanism modules/bruce_launcher/bruce_launcher_app.c and
 * modules/webui/webui_app.c already use - so all validation/flash logic
 * lives in one place and this file only ever builds a command line. */
static bruce_result_t partition_manager_gui__run(const char *command_line) {
    int launched = app_runner__run_command(command_line, BRUCE_LAUNCH_BACKGROUND);
    if (launched <= 0) return (bruce_result_t)launched;

    bruce_process_status_t status;
    for (;;) {
        bruce_result_t waited = process__wait_status((bruce_process_id_t)launched, 100, &status);
        if (waited == BRUCE_OK) break;
        if (waited == BRUCE_ERR_TIMEOUT) continue;
        return waited;
    }
    if (status.reason != BRUCE_PROCESS_EXITED) return BRUCE_ERR_INTERNAL;
    return status.exit_code == 0 ? BRUCE_OK : BRUCE_ERR_IO;
}

static void partition_manager_gui__show_error(const char *action, bruce_result_t result) {
    char message[80];
    snprintf(message, sizeof(message), "%s failed (%d)", action, result);
    (void)dialog__message(BRUCE_DIALOG_ERROR, "Partitions", message);
}

static bruce_result_t partition_manager_gui__offer_reboot(void) {
    if (!partition_manager__reboot_required()) return BRUCE_OK;
    const bruce_dialog_choice_t choices[] = {
        {.label = "Reboot now", .value = "yes"},
        {.label = "Later",      .value = "no" },
    };
    size_t selected = 1;
    bruce_result_t result = dialog__choice(
        "Partitions", "Changes are staged. Reboot now to apply them?", choices, 2, &selected, NULL
    );
    if (result != BRUCE_OK || selected != 0) return BRUCE_OK;
    result = partition_manager_gui__run("bparted reboot");
    if (result != BRUCE_OK) partition_manager_gui__show_error("Reboot", result);
    return BRUCE_OK;
}

static bruce_result_t partition_manager_gui__pick_size(char *out_text, size_t out_size) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "64K",      .value = "64K"   },
        {.label = "128K",     .value = "128K"  },
        {.label = "256K",     .value = "256K"  },
        {.label = "512K",     .value = "512K"  },
        {.label = "1M",       .value = "1M"    },
        {.label = "2M",       .value = "2M"    },
        {.label = "4M",       .value = "4M"    },
        {.label = "Custom...", .value = "custom"},
        {.label = "Cancel",   .value = "cancel"},
    };
    size_t count = sizeof(choices) / sizeof(choices[0]);
    size_t selected = 0;
    bruce_result_t result = dialog__choice("Create partition", "Size", choices, count, &selected, NULL);
    if (result != BRUCE_OK) return result;
    if (selected == count - 1) return BRUCE_ERR_CANCELLED;
    if (selected != count - 2) {
        snprintf(out_text, out_size, "%s", choices[selected].value);
        return BRUCE_OK;
    }
    return dialog__text_input("Create partition", "Size (e.g. 768K, 3M)", "", false, out_text, out_size);
}

static bruce_result_t partition_manager_gui__create(void) {
    const bruce_dialog_choice_t kind_choices[] = {
        {.label = "Swap",                     .value = "swap"           },
        {.label = "LittleFS (root)",          .value = "littlefs-root"  },
        {.label = "Custom label (LittleFS)",  .value = "littlefs-custom"},
        {.label = "Cancel",                   .value = "cancel"         },
    };
    size_t count = sizeof(kind_choices) / sizeof(kind_choices[0]);
    size_t selected = 0;
    bruce_result_t result = dialog__choice("Create partition", "Type", kind_choices, count, &selected, NULL);
    if (result != BRUCE_OK) return result;
    if (selected == count - 1) return BRUCE_ERR_CANCELLED;

    char label[BRUCE_PARTITION_LABEL_MAX];
    const char *kind_arg;
    if (selected == 0) {
        snprintf(label, sizeof(label), "swap");
        kind_arg = "swap";
    } else if (selected == 1) {
        snprintf(label, sizeof(label), "littlefs");
        kind_arg = "littlefs";
    } else {
        label[0] = '\0';
        result = dialog__text_input(
            "Create partition", "Label (letters/numbers/_/-)", "", false, label, sizeof(label)
        );
        if (result != BRUCE_OK) return result;
        if (label[0] == '\0' || strpbrk(label, " \t\"'\\") != NULL) {
            (void)dialog__message(BRUCE_DIALOG_ERROR, "Partitions", "Invalid label");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        kind_arg = "littlefs";
    }

    char size_text[24];
    result = partition_manager_gui__pick_size(size_text, sizeof(size_text));
    if (result != BRUCE_OK) return result;

    char command[96];
    snprintf(command, sizeof(command), "bparted create %s %s %s", label, kind_arg, size_text);
    result = partition_manager_gui__run(command);
    if (result != BRUCE_OK) {
        partition_manager_gui__show_error("Create", result);
        return BRUCE_OK;
    }
    (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Partitions", "Partition staged.");
    return partition_manager_gui__offer_reboot();
}

static bruce_result_t partition_manager_gui__manage(const char *label) {
    bool is_root = strcmp(label, "littlefs") == 0;
    const bruce_dialog_choice_t choices[] = {
        {.label = "Format", .value = "format"},
        {.label = "Delete", .value = "delete"},
        {.label = "Back",   .value = "back"  },
    };
    size_t selected = 0;
    bruce_result_t result = dialog__choice(label, "Action", choices, 3, &selected, NULL);
    if (result != BRUCE_OK || selected == 2) return BRUCE_OK;

    if (selected == 1 && is_root) {
        (void)dialog__message(
            BRUCE_DIALOG_ERROR, "Partitions", "The root 'littlefs' partition cannot be deleted; format it instead."
        );
        return BRUCE_OK;
    }

    const bruce_dialog_choice_t confirm_actions[] = {
        {.label = selected == 0 ? "Format" : "Delete", .value = "confirm"},
        {.label = "Cancel",                             .value = "cancel"},
    };
    size_t confirm_selected = 1;
    char message[64];
    if (selected == 0) snprintf(message, sizeof(message), "Erase and reformat '%s'?", label);
    else snprintf(message, sizeof(message), "Delete '%s'?", label);
    result = dialog__choice("Partitions", message, confirm_actions, 2, &confirm_selected, NULL);
    if (result != BRUCE_OK || confirm_selected != 0) return BRUCE_OK;

    char command[64];
    snprintf(command, sizeof(command), "bparted %s %s", selected == 0 ? "format" : "delete", label);
    result = partition_manager_gui__run(command);
    if (result != BRUCE_OK) {
        partition_manager_gui__show_error(selected == 0 ? "Format" : "Delete", result);
        return BRUCE_OK;
    }
    (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Partitions", "Change staged.");
    return partition_manager_gui__offer_reboot();
}

static bruce_result_t partition_manager_gui__main_menu(void) {
    bruce_partition_entry_t entries[PARTITION_MANAGER_GUI_MAX_ENTRIES];
    size_t entry_count = 0;
    (void)partition_manager__list(entries, PARTITION_MANAGER_GUI_MAX_ENTRIES, &entry_count);
    if (entry_count > PARTITION_MANAGER_GUI_MAX_ENTRIES) entry_count = PARTITION_MANAGER_GUI_MAX_ENTRIES;

    char entry_labels[PARTITION_MANAGER_GUI_MAX_ENTRIES][40];
    bruce_dialog_choice_t choices[PARTITION_MANAGER_GUI_MAX_ENTRIES + 3];
    size_t count = 0;
    for (size_t i = 0; i < entry_count; ++i) {
        char size_text[16];
        partition_manager_common__format_size(entries[i].size, size_text, sizeof(size_text));
        snprintf(
            entry_labels[count], sizeof(entry_labels[count]), "%s (%s, %s)", entries[i].label,
            partition_manager_common__kind_name(entries[i].kind), size_text
        );
        choices[count].label = entry_labels[count];
        choices[count].value = entries[i].label;
        count++;
    }
    choices[count].label = "Create partition...";
    choices[count].value = "__create";
    count++;
    if (partition_manager__reboot_required()) {
        choices[count].label = "Reboot now";
        choices[count].value = "__reboot";
        count++;
    }
    choices[count].label = "Back";
    choices[count].value = "__back";
    count++;

    size_t selected = 0;
    bruce_result_t result =
        dialog__choice("Partitions", "Select a partition or action", choices, count, &selected, NULL);
    if (result != BRUCE_OK) return result;

    const char *value = choices[selected].value;
    if (strcmp(value, "__back") == 0) return BRUCE_ERR_CANCELLED;
    if (strcmp(value, "__create") == 0) return partition_manager_gui__create();
    if (strcmp(value, "__reboot") == 0) {
        result = partition_manager_gui__run("bparted reboot");
        if (result != BRUCE_OK) partition_manager_gui__show_error("Reboot", result);
        return BRUCE_OK;
    }
    return partition_manager_gui__manage(value);
}

int partition_manager_gui_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (;;) {
        bruce_result_t result = partition_manager_gui__main_menu();
        if (result == BRUCE_ERR_CANCELLED) return 0;
        if (result != BRUCE_OK) {
            partition_manager_gui__show_error("Partitions", result);
            return result;
        }
    }
}
