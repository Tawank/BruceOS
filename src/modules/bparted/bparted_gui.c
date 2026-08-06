#include "bparted_gui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/partition_manager.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"

#include "bparted_common.h"

#define BPARTED_GUI_MAX_ENTRIES 8
/* Worst case: every committed entry plus every pending entry are disjoint
 * (all creates and all deletes staged at once), plus the fixed action rows
 * (Create/Apply/Cancel/Reboot/Back). */
#define BPARTED_GUI_MAX_ROWS (BPARTED_GUI_MAX_ENTRIES * 2 + 5)

/* Every mutation drives the "bparted" CLI mode (bparted_cli.c) via
 * app_runner__run_command(), the same shell-command-from-GUI mechanism
 * modules/bruce_launcher/bruce_launcher_app.c and modules/webui/webui_app.c
 * already use - so all validation/flash logic lives in one place and this
 * file only ever builds a command line. Running "bparted ..." without
 * GUI=1 (the default for a child launched this way) routes back into the
 * CLI, not this GUI, via bparted_app.c's dispatch.
 *
 * Create/Delete/Format here all go through "bparted stage ..." - which
 * only edits the in-RAM working table (core_sdk/partition_manager.h's
 * partition_manager__list()) - never "bparted create/delete/format", which
 * would write to flash immediately. Only the main menu's "Apply changes"
 * ("bparted apply") ever commits; "Cancel changes" ("bparted cancel")
 * drops every staged edit instead. Read-only queries (list()/
 * list_committed()/has_pending_changes()/reboot_required()) are cheap and
 * safe to call directly from this process - see the plan this app was
 * built from - so only state-changing calls go through a command line. */
static bruce_result_t bparted_gui__run(const char *command_line) {
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

static void bparted_gui__show_error(const char *action, bruce_result_t result) {
    char message[80];
    snprintf(message, sizeof(message), "%s failed (%d)", action, result);
    (void)dialog__message(BRUCE_DIALOG_ERROR, "Partitions", message);
}

static bool bparted_gui__find(
    const bruce_partition_entry_t *entries, size_t count, const char *label, size_t *out_index
) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].label, label) == 0) {
            if (out_index != NULL) *out_index = i;
            return true;
        }
    }
    return false;
}

static bruce_result_t bparted_gui__offer_reboot(void) {
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
    result = bparted_gui__run("bparted reboot");
    if (result != BRUCE_OK) bparted_gui__show_error("Reboot", result);
    return BRUCE_OK;
}

/* Shared by the main menu's "Apply changes" button and confirm_exit()'s
 * "Apply and exit" choice. No confirmation dialog of its own - Apply only
 * writes what Create/Delete/Format already staged (and showed their own
 * "staged" confirmation for), so it isn't a surprising action the way
 * discarding staged work is. */
static void bparted_gui__do_apply(void) {
    bruce_result_t result = bparted_gui__run("bparted apply");
    if (result != BRUCE_OK) {
        bparted_gui__show_error("Apply", result);
        return;
    }
    (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Partitions", "Changes applied.");
    (void)bparted_gui__offer_reboot();
}

/* Shared by the main menu's "Cancel changes" button (which confirms first,
 * see below) and confirm_exit()'s "Discard changes and exit" choice (whose
 * own wording already is the confirmation). */
static void bparted_gui__do_cancel(void) {
    bruce_result_t result = bparted_gui__run("bparted cancel");
    if (result != BRUCE_OK) {
        bparted_gui__show_error("Cancel", result);
        return;
    }
    (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Partitions", "Staged changes discarded.");
}

static void bparted_gui__cancel(void) {
    const bruce_dialog_choice_t confirm_actions[] = {
        {.label = "Discard changes", .value = "confirm"},
        {.label = "Keep editing",    .value = "cancel" },
    };
    size_t selected = 1;
    bruce_result_t result =
        dialog__choice("Partitions", "Discard every staged change?", confirm_actions, 2, &selected, NULL);
    if (result != BRUCE_OK || selected != 0) return;
    bparted_gui__do_cancel();
}

static bruce_result_t bparted_gui__pick_size(char *out_text, size_t out_size) {
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

/* Every early return here is the user backing out of one step of this
 * wizard (ESC, "Cancel") - that's normal, expected navigation, not an app
 * error, so this never propagates anything to the caller; it always just
 * returns to the main menu. */
static void bparted_gui__create(void) {
    const bruce_dialog_choice_t kind_choices[] = {
        {.label = "Swap",                    .value = "swap"           },
        {.label = "LittleFS (root)",         .value = "littlefs-root"  },
        {.label = "Custom label (LittleFS)", .value = "littlefs-custom"},
        {.label = "Cancel",                  .value = "cancel"         },
    };
    size_t count = sizeof(kind_choices) / sizeof(kind_choices[0]);
    size_t selected = 0;
    bruce_result_t result = dialog__choice("Create partition", "Type", kind_choices, count, &selected, NULL);
    if (result != BRUCE_OK || selected == count - 1) return;

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
        if (result != BRUCE_OK) return;
        if (label[0] == '\0' || strpbrk(label, " \t\"'\\") != NULL) {
            (void)dialog__message(BRUCE_DIALOG_ERROR, "Partitions", "Invalid label");
            return;
        }
        kind_arg = "littlefs";
    }

    char size_text[24];
    result = bparted_gui__pick_size(size_text, sizeof(size_text));
    if (result != BRUCE_OK) return;

    char command[96];
    snprintf(command, sizeof(command), "bparted stage create %s %s %s", label, kind_arg, size_text);
    result = bparted_gui__run(command);
    if (result != BRUCE_OK) {
        bparted_gui__show_error("Create", result);
        return;
    }
    (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Partitions", "Staged. Apply from the main menu to write it.");
}

/* `is_new`: staged this session, not on flash at all yet (Format makes no
 * sense - it's already going to be freshly formatted on creation).
 * `is_staged_delete`: on flash, but staged for removal - list() no longer
 * has it, so there's nothing left here to Format/Delete; only Apply/Cancel
 * (from the main menu) can resolve it. Like create(), this always returns
 * to the main menu itself; it never propagates cancellation or errors. */
static void bparted_gui__manage(const char *label, bool is_new, bool is_staged_delete) {
    if (is_staged_delete) {
        char message[96];
        snprintf(
            message, sizeof(message), "'%s' is staged for deletion. Apply or Cancel from the main menu.", label
        );
        (void)dialog__message(BRUCE_DIALOG_INFO, "Partitions", message);
        return;
    }

    bool is_root = strcmp(label, "littlefs") == 0;
    bruce_dialog_choice_t choices[3];
    size_t choice_count = 0;
    if (!is_new) {
        choices[choice_count].label = "Format";
        choices[choice_count].value = "format";
        choice_count++;
    }
    choices[choice_count].label = "Delete";
    choices[choice_count].value = "delete";
    choice_count++;
    choices[choice_count].label = "Back";
    choices[choice_count].value = "back";
    choice_count++;

    size_t selected = choice_count - 1;
    bruce_result_t result = dialog__choice(label, "Action", choices, choice_count, &selected, NULL);
    if (result != BRUCE_OK) return;
    const char *value = choices[selected].value;
    if (strcmp(value, "back") == 0) return;

    bool is_format = strcmp(value, "format") == 0;
    if (!is_format && is_root) {
        (void)dialog__message(
            BRUCE_DIALOG_ERROR, "Partitions", "The root 'littlefs' partition cannot be deleted; format it instead."
        );
        return;
    }

    const bruce_dialog_choice_t confirm_actions[] = {
        {.label = is_format ? "Format" : "Delete", .value = "confirm"},
        {.label = "Cancel",                        .value = "cancel"},
    };
    size_t confirm_selected = 1;
    char message[64];
    if (is_format) snprintf(message, sizeof(message), "Stage a format of '%s'?", label);
    else snprintf(message, sizeof(message), "Stage deletion of '%s'?", label);
    result = dialog__choice("Partitions", message, confirm_actions, 2, &confirm_selected, NULL);
    if (result != BRUCE_OK || confirm_selected != 0) return;

    char command[64];
    snprintf(command, sizeof(command), "bparted stage %s %s", is_format ? "format" : "delete", label);
    result = bparted_gui__run(command);
    if (result != BRUCE_OK) {
        bparted_gui__show_error(is_format ? "Format" : "Delete", result);
        return;
    }
    (void)dialog__message(BRUCE_DIALOG_SUCCESS, "Partitions", "Staged. Apply from the main menu to write it.");
}

/* The root list's own Back/ESC (as opposed to ESC inside a submenu, which
 * just returns here - see bparted_gui__main_menu()). Leaving with unsaved
 * or unapplied work is surprising, so this is the one place that warns and
 * offers to resolve it before actually exiting; "nothing staged, nothing
 * to apply" exits immediately with no prompt. Returns BRUCE_ERR_CANCELLED
 * only once the user has confirmed they want to leave. */
static bruce_result_t bparted_gui__confirm_exit(void) {
    bool dirty = partition_manager__has_pending_changes();
    bool reboot = partition_manager__reboot_required();
    if (!dirty && !reboot) return BRUCE_ERR_CANCELLED;

    bruce_dialog_choice_t choices[4];
    size_t count = 0;
    if (dirty) {
        choices[count].label = "Apply and exit";
        choices[count].value = "apply";
        count++;
        choices[count].label = "Discard changes and exit";
        choices[count].value = "discard";
        count++;
    }
    if (reboot) {
        choices[count].label = "Reboot now";
        choices[count].value = "reboot";
        count++;
    }
    choices[count].label = "Stay";
    choices[count].value = "stay";
    count++;

    size_t selected = count - 1;
    const char *message =
        dirty ? "Unsaved partition changes will be lost." : "Partition changes are staged; reboot to apply them.";
    bruce_result_t result = dialog__choice("Exit Partitions?", message, choices, count, &selected, NULL);
    /* Backing out of the warning itself (another ESC): stay, don't exit. */
    if (result != BRUCE_OK) return BRUCE_OK;

    const char *value = choices[selected].value;
    if (strcmp(value, "apply") == 0) {
        bparted_gui__do_apply();
        return BRUCE_ERR_CANCELLED;
    }
    if (strcmp(value, "discard") == 0) {
        bparted_gui__do_cancel();
        return BRUCE_ERR_CANCELLED;
    }
    if (strcmp(value, "reboot") == 0) {
        bruce_result_t reboot_result = bparted_gui__run("bparted reboot");
        /* Only reached if the reboot itself failed to start; a successful
         * one never returns. */
        if (reboot_result != BRUCE_OK) bparted_gui__show_error("Reboot", reboot_result);
        return BRUCE_OK;
    }
    return BRUCE_OK; /* "stay" */
}

/* Builds one flat list showing both the current (list_committed()) and
 * pending (list()) layouts at once, diffed against each other: every row
 * is a pending-table entry, tagged "[new]"/"[format]" when it differs from
 * what's committed, plus one "[delete]" row per committed entry pending()
 * no longer has. Rows unaffected by any staged edit show plainly, with no
 * tag - current and pending agree there. */
static bruce_result_t bparted_gui__main_menu(void) {
    bruce_partition_entry_t committed[BPARTED_GUI_MAX_ENTRIES];
    size_t committed_count = 0;
    (void)partition_manager__list_committed(committed, BPARTED_GUI_MAX_ENTRIES, &committed_count);
    if (committed_count > BPARTED_GUI_MAX_ENTRIES) committed_count = BPARTED_GUI_MAX_ENTRIES;

    bruce_partition_entry_t pending[BPARTED_GUI_MAX_ENTRIES];
    size_t pending_count = 0;
    (void)partition_manager__list(pending, BPARTED_GUI_MAX_ENTRIES, &pending_count);
    if (pending_count > BPARTED_GUI_MAX_ENTRIES) pending_count = BPARTED_GUI_MAX_ENTRIES;

    bool dirty = partition_manager__has_pending_changes();
    bool reboot = partition_manager__reboot_required();

    /* Worst case: 16-char label + " (" + 8-char "littlefs" + ", " +
     * up to 15-char size_text + ")" + up to 9-char " [format]" tag + NUL =
     * 54; rounded up for headroom. */
    char entry_labels[BPARTED_GUI_MAX_ROWS][64];
    bruce_dialog_choice_t choices[BPARTED_GUI_MAX_ROWS];
    size_t count = 0;

    for (size_t i = 0; i < pending_count && count < BPARTED_GUI_MAX_ENTRIES; ++i) {
        /* Copied into a plain, statically-sized local first (rather than
         * formatting straight from pending[i].label): some GCC versions
         * can't bound a %s read from a struct-array element reached via a
         * runtime index and assume an unbounded string, flagging a bogus
         * -Wformat-truncation below despite label's real 17-byte bound. A
         * flat local array sidesteps that ambiguity entirely. */
        char label[BRUCE_PARTITION_LABEL_MAX];
        memcpy(label, pending[i].label, sizeof(label));
        label[sizeof(label) - 1] = '\0';

        bool exists_committed = bparted_gui__find(committed, committed_count, pending[i].label, NULL);
        const char *tag = !exists_committed ? " [new]" : (pending[i].format_pending ? " [format]" : "");

        char size_text[16];
        bparted_common__format_size(pending[i].size, size_text, sizeof(size_text));
        snprintf(
            entry_labels[count], sizeof(entry_labels[count]), "%s (%s, %s)%s", label,
            bparted_common__kind_name(pending[i].kind), size_text, tag
        );
        choices[count].label = entry_labels[count];
        choices[count].value = pending[i].label;
        count++;
    }

    for (size_t i = 0; i < committed_count && count < BPARTED_GUI_MAX_ENTRIES * 2; ++i) {
        if (bparted_gui__find(pending, pending_count, committed[i].label, NULL)) continue;

        char label[BRUCE_PARTITION_LABEL_MAX];
        memcpy(label, committed[i].label, sizeof(label));
        label[sizeof(label) - 1] = '\0';

        char size_text[16];
        bparted_common__format_size(committed[i].size, size_text, sizeof(size_text));
        snprintf(
            entry_labels[count], sizeof(entry_labels[count]), "%s (%s, %s) [delete]", label,
            bparted_common__kind_name(committed[i].kind), size_text
        );
        choices[count].label = entry_labels[count];
        choices[count].value = committed[i].label;
        count++;
    }

    choices[count].label = "Create partition...";
    choices[count].value = "__create";
    count++;
    if (dirty) {
        choices[count].label = "Apply changes";
        choices[count].value = "__apply";
        count++;
        choices[count].label = "Cancel changes";
        choices[count].value = "__cancel";
        count++;
    }
    if (reboot) {
        choices[count].label = "Reboot now";
        choices[count].value = "__reboot";
        count++;
    }
    choices[count].label = "Back";
    choices[count].value = "__back";
    count++;

    const char *message;
    if (dirty) message = "Unsaved changes: Apply or Cancel below";
    else if (reboot) message = "Staged changes: reboot to apply them";
    else message = "Select a partition or action";

    size_t selected = 0;
    bruce_result_t result = dialog__choice("Partitions", message, choices, count, &selected, NULL);
    if (result != BRUCE_OK) {
        /* Physical Back/ESC pressed on the root list itself: this is the
         * only place that can actually end the app (see confirm_exit()). */
        return bparted_gui__confirm_exit();
    }

    const char *value = choices[selected].value;
    if (strcmp(value, "__back") == 0) return bparted_gui__confirm_exit();
    if (strcmp(value, "__create") == 0) {
        bparted_gui__create();
        return BRUCE_OK;
    }
    if (strcmp(value, "__apply") == 0) {
        bparted_gui__do_apply();
        return BRUCE_OK;
    }
    if (strcmp(value, "__cancel") == 0) {
        bparted_gui__cancel();
        return BRUCE_OK;
    }
    if (strcmp(value, "__reboot") == 0) {
        bruce_result_t reboot_result = bparted_gui__run("bparted reboot");
        if (reboot_result != BRUCE_OK) bparted_gui__show_error("Reboot", reboot_result);
        return BRUCE_OK;
    }

    /* Anything else is a partition label (a row built above). */
    bool in_pending = bparted_gui__find(pending, pending_count, value, NULL);
    bool in_committed = bparted_gui__find(committed, committed_count, value, NULL);
    bparted_gui__manage(value, in_pending && !in_committed, !in_pending);
    return BRUCE_OK;
}

int bparted_gui__main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (;;) {
        bruce_result_t result = bparted_gui__main_menu();
        if (result == BRUCE_ERR_CANCELLED) return 0;
        if (result != BRUCE_OK) {
            bparted_gui__show_error("Partitions", result);
            return result;
        }
    }
}
