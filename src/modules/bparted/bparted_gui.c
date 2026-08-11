#include "bparted_gui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/device.h"
#include "core_sdk/dialog.h"
#include "core_sdk/partition_manager.h"
#include "core_sdk/result.h"

#include "bparted_common.h"

/* One screen showing both layouts at once - what the device is running
 * right now, and what it will be running after the next boot - over a fixed
 * row of actions (Create/Delete/Format/Apply/Cancel/Exit). That pairing is
 * the whole point of the app: every edit only ever moves the second list,
 * and the first one cannot change until the device reboots, so the two
 * lists side by side are the answer to "what did I just change, and has it
 * happened yet?".
 *
 * Everything here calls core_sdk/partition_manager.h directly. Editing is
 * restricted to built-in modules and this is one, so there is nothing to
 * gain from bouncing each action through a "bparted ..." child process: a
 * stage_*() call is a mutex and a memcpy, while spawning a process to make
 * it costs a task, a stack and a wait. The CLI front end is a peer of this
 * file, not a back end for it.
 *
 * Nothing on this screen touches flash except Apply, and nothing touches
 * user data at all until the next boot - see core_sdk/partition_manager.h. */

/* Longest "<label> <size> [<change>]" row rendered below. */
#define BPARTED_GUI_ROW_TEXT 40

/* Partition rows the page can show at once: the running layout plus the
 * next-boot one, which is the longer of the two (see BPARTED_PLANNED_MAX). */
#define BPARTED_GUI_ENTRY_ROWS (BPARTED_LAYOUT_MAX + BPARTED_PLANNED_MAX)

/* Those, plus the three headings, the "(same as above)" placeholder, and
 * the seven action rows. */
#define BPARTED_GUI_MAX_ROWS (BPARTED_GUI_ENTRY_ROWS + 11)

/* Below this a partition is too small to be worth offering to create; it is
 * also the flash sector size every size is rounded up to. */
#define BPARTED_GUI_MIN_NEW_BYTES 4096u

typedef enum {
    /* Headings and the layout rows themselves: selecting one does nothing
     * but redraw. The two lists are a readout, not a menu - every edit goes
     * through a named action below, so there is never a hidden gesture that
     * changes the layout. */
    BPARTED_GUI_ACTION_NONE,
    BPARTED_GUI_ACTION_CREATE,
    BPARTED_GUI_ACTION_DELETE,
    BPARTED_GUI_ACTION_FORMAT,
    BPARTED_GUI_ACTION_APPLY,
    BPARTED_GUI_ACTION_CANCEL,
    BPARTED_GUI_ACTION_REBOOT,
    BPARTED_GUI_ACTION_EXIT,
} bparted_gui_action_t;

typedef struct {
    bruce_partition_entry_t current[BPARTED_LAYOUT_MAX];
    size_t current_count;
    bruce_partition_entry_t planned[BPARTED_PLANNED_MAX];
    size_t planned_count;
    bruce_partition_status_t status;

    char row_text[BPARTED_GUI_ENTRY_ROWS][BPARTED_GUI_ROW_TEXT];
    size_t text_count;
    bruce_dialog_choice_t choices[BPARTED_GUI_MAX_ROWS];
    uint8_t actions[BPARTED_GUI_MAX_ROWS];
    size_t row_count;

    char summary[64];
} bparted_gui_page_t;

static void bparted_gui__notify(bruce_dialog_kind_t kind, const char *message) {
    (void)dialog__message(kind, "Partitions", message);
}

static void bparted_gui__report(const char *action, bruce_result_t result) {
    char message[128];
    snprintf(message, sizeof(message), "%s: %s", action, bparted_common__error_text(result));
    bparted_gui__notify(BRUCE_DIALOG_ERROR, message);
}

/* A two-way "go ahead / back out" prompt that always starts on backing out,
 * so a stray confirm press can never be what destroys someone's data. */
static bool bparted_gui__confirm(const char *title, const char *message, const char *confirm_label) {
    const bruce_dialog_choice_t choices[] = {
        {.label = confirm_label, .value = "confirm"},
        {.label = "Back",        .value = "back"   },
    };
    size_t selected = 1;
    bruce_result_t result = dialog__choice_launcher(title, message, choices, 2, &selected);
    return result == BRUCE_OK && selected == 0;
}

/* "<label> <size> [<change>]", with the root entry marked by the path it is
 * mounted at. Used for both lists and for the Delete/Format pickers, so a
 * partition looks the same everywhere it appears. */
static void bparted_gui__format_row(const bruce_partition_entry_t *entry, char *out, size_t capacity) {
    char size_text[16];
    bparted_common__format_size(entry->size, size_text, sizeof(size_text));

    const char *state = bparted_common__state_name(entry->state);
    char change[12];
    if (state[0] != '\0') snprintf(change, sizeof(change), " [%s]", state);
    else change[0] = '\0';

    /* The label is padded to 12 so the size column lines up on the common
     * short labels, and capped at its real 16-char maximum so a long one is
     * still shown whole. Every conversion is bounded rather than left open:
     * it keeps the row narrow enough to read on a small display, and it is
     * what lets the compiler prove the whole row fits in `capacity`. */
    snprintf(
        out, capacity, "%-12.16s %7.7s%s%.9s", entry->label, size_text, entry->is_root ? " /" : "", change
    );
}

static bool bparted_gui__has_label(const bruce_partition_entry_t *entries, size_t count, const char *label) {
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].state == BRUCE_PARTITION_STATE_DELETED) continue;
        if (strcmp(entries[i].label, label) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Page                                                                      */
/* ------------------------------------------------------------------------ */

static void bparted_gui__add_row(bparted_gui_page_t *page, const char *label, bparted_gui_action_t action) {
    if (page->row_count >= BPARTED_GUI_MAX_ROWS) return;
    page->choices[page->row_count].label = label;
    page->choices[page->row_count].value = label;
    page->actions[page->row_count] = (uint8_t)action;
    page->row_count++;
}

static void bparted_gui__add_entry_row(bparted_gui_page_t *page, const bruce_partition_entry_t *entry) {
    if (page->text_count >= BPARTED_GUI_ENTRY_ROWS) return;
    char *text = page->row_text[page->text_count];
    bparted_gui__format_row(entry, text, BPARTED_GUI_ROW_TEXT);
    page->text_count++;
    bparted_gui__add_row(page, text, BPARTED_GUI_ACTION_NONE);
}

/* Re-reads both layouts and rebuilds every row. Called before each frame
 * rather than patched incrementally: the tables are a few hundred bytes and
 * a redraw only happens when someone pressed a key, so re-reading is both
 * cheaper to reason about and immune to the page drifting out of sync with
 * what core actually staged. */
static bruce_result_t bparted_gui__refresh(bparted_gui_page_t *page) {
    page->row_count = 0;
    page->text_count = 0;
    page->current_count = 0;
    page->planned_count = 0;

    bruce_result_t result = bparted_common__clamp_list(
        partition_manager__list_current(page->current, BPARTED_LAYOUT_MAX, &page->current_count),
        &page->current_count,
        BPARTED_LAYOUT_MAX
    );
    if (result != BRUCE_OK) return result;

    result = bparted_common__clamp_list(
        partition_manager__list_planned(page->planned, BPARTED_PLANNED_MAX, &page->planned_count),
        &page->planned_count,
        BPARTED_PLANNED_MAX
    );
    if (result != BRUCE_OK) return result;

    result = partition_manager__status(&page->status);
    if (result != BRUCE_OK) return result;

    bool changed = page->status.has_pending_changes || page->status.reboot_required;

    bparted_gui__add_row(page, "-- Running now --", BPARTED_GUI_ACTION_NONE);
    for (size_t i = 0; i < page->current_count; ++i) bparted_gui__add_entry_row(page, &page->current[i]);

    bparted_gui__add_row(page, "-- After next boot --", BPARTED_GUI_ACTION_NONE);
    if (changed) {
        for (size_t i = 0; i < page->planned_count; ++i) bparted_gui__add_entry_row(page, &page->planned[i]);
    } else {
        /* Identical to the list above by definition (nothing staged, nothing
         * committed since boot), so repeating it would only cost screen. */
        bparted_gui__add_row(page, "  (unchanged)", BPARTED_GUI_ACTION_NONE);
    }

    bparted_gui__add_row(page, "-- Actions --", BPARTED_GUI_ACTION_NONE);
    bparted_gui__add_row(page, "Create", BPARTED_GUI_ACTION_CREATE);
    bparted_gui__add_row(page, "Delete", BPARTED_GUI_ACTION_DELETE);
    bparted_gui__add_row(page, "Format", BPARTED_GUI_ACTION_FORMAT);
    bparted_gui__add_row(page, "Apply", BPARTED_GUI_ACTION_APPLY);
    bparted_gui__add_row(page, "Cancel", BPARTED_GUI_ACTION_CANCEL);
    if (page->status.reboot_required) bparted_gui__add_row(page, "Reboot now", BPARTED_GUI_ACTION_REBOOT);
    bparted_gui__add_row(page, "Exit", BPARTED_GUI_ACTION_EXIT);

    char free_text[16];
    char total_text[16];
    bparted_common__format_size(page->status.unallocated_bytes, free_text, sizeof(free_text));
    bparted_common__format_size(page->status.total_bytes, total_text, sizeof(total_text));
    if (page->status.has_pending_changes) {
        snprintf(
            page->summary, sizeof(page->summary), "Not applied yet - %s free of %s", free_text, total_text
        );
    } else if (page->status.reboot_required) {
        snprintf(page->summary, sizeof(page->summary), "Applied - reboot to take effect");
    } else {
        snprintf(page->summary, sizeof(page->summary), "%s free of %s", free_text, total_text);
    }
    return BRUCE_OK;
}

/* ------------------------------------------------------------------------ */
/* Actions                                                                   */
/* ------------------------------------------------------------------------ */

static void bparted_gui__reboot(void) {
    if (!bparted_gui__confirm("Reboot", "Reboot now to apply the saved layout?", "Reboot now")) return;
    bruce_result_t result = device__restart(500);
    /* Only reached when the restart request itself was refused; a successful
     * one never comes back. */
    if (result != BRUCE_OK) bparted_gui__report("Reboot", result);
}

static bool bparted_gui__commit(void) {
    bruce_result_t result = partition_manager__commit();
    if (result != BRUCE_OK) {
        bparted_gui__report("Apply", result);
        return false;
    }
    bparted_gui__notify(BRUCE_DIALOG_SUCCESS, "Saved. It takes effect on the next boot.");
    return true;
}

static void bparted_gui__apply(const bparted_gui_page_t *page) {
    if (!page->status.has_pending_changes) {
        bparted_gui__notify(BRUCE_DIALOG_INFO, "Nothing to apply.");
        return;
    }
    if (!bparted_gui__confirm(
            "Apply",
            "Save this layout? Partitions marked new, delete or format are erased on the next boot.",
            "Apply"
        )) {
        return;
    }
    if (!bparted_gui__commit()) return;
    bparted_gui__reboot();
}

static void bparted_gui__cancel(const bparted_gui_page_t *page) {
    if (!page->status.has_pending_changes) {
        bparted_gui__notify(BRUCE_DIALOG_INFO, "Nothing to cancel.");
        return;
    }
    if (!bparted_gui__confirm("Cancel", "Throw away every change that has not been applied?", "Discard")) {
        return;
    }
    bruce_result_t result = partition_manager__discard();
    if (result != BRUCE_OK) {
        bparted_gui__report("Cancel", result);
        return;
    }
    bparted_gui__notify(BRUCE_DIALOG_SUCCESS, "Changes discarded.");
}

/* Offers the preset sizes that would actually fit, then the whole remaining
 * gap, then free text - so the quick path cannot pick something
 * stage_create() is only going to reject. */
static bool bparted_gui__pick_size(uint64_t max_bytes, uint64_t *out_bytes) {
    static const uint64_t presets[] = {
        64ull * 1024ull,
        128ull * 1024ull,
        256ull * 1024ull,
        512ull * 1024ull,
        1024ull * 1024ull,
        2048ull * 1024ull,
        4096ull * 1024ull,
    };
    enum { BPARTED_GUI_PRESETS = sizeof(presets) / sizeof(presets[0]) };

    char labels[BPARTED_GUI_PRESETS][16];
    bruce_dialog_choice_t choices[BPARTED_GUI_PRESETS + 3];
    size_t count = 0;
    for (size_t i = 0; i < BPARTED_GUI_PRESETS && presets[i] <= max_bytes; ++i) {
        bparted_common__format_size(presets[i], labels[count], sizeof(labels[count]));
        choices[count].label = labels[count];
        choices[count].value = labels[count];
        count++;
    }
    size_t preset_count = count;

    char max_label[32];
    char max_text[16];
    bparted_common__format_size(max_bytes, max_text, sizeof(max_text));
    snprintf(max_label, sizeof(max_label), "All free space (%.7s)", max_text);
    choices[count].label = max_label;
    choices[count].value = "max";
    count++;

    choices[count].label = "Custom...";
    choices[count].value = "custom";
    count++;
    choices[count].label = "Back";
    choices[count].value = "back";
    count++;

    size_t selected = 0;
    if (dialog__choice("New partition", "Size", choices, count, &selected) != BRUCE_OK) return false;
    if (selected == count - 1) return false;
    if (selected < preset_count) {
        *out_bytes = presets[selected];
        return true;
    }
    if (selected == preset_count) {
        *out_bytes = max_bytes;
        return true;
    }

    char text[24];
    text[0] = '\0';
    if (dialog__text_input("New partition", "Size (e.g. 768K, 3M)", "", false, text, sizeof(text)) !=
        BRUCE_OK) {
        return false;
    }
    if (!bparted_common__parse_size(text, out_bytes)) {
        bparted_gui__notify(BRUCE_DIALOG_ERROR, "Size must look like 512K, 2M, or a plain byte count.");
        return false;
    }
    return true;
}

static void bparted_gui__create(const bparted_gui_page_t *page) {
    if (page->status.max_new_size < BPARTED_GUI_MIN_NEW_BYTES) {
        bparted_gui__notify(
            BRUCE_DIALOG_WARNING, "No room for another partition. Delete one first, then Apply."
        );
        return;
    }

    /* A second swap partition is meaningless - core/memory looks for exactly
     * one - and stage_create() would reject it, so it is not offered. */
    bool offer_swap = !bparted_gui__has_label(page->planned, page->planned_count, BRUCE_PARTITION_SWAP_LABEL);

    bruce_dialog_choice_t kinds[3];
    size_t kind_count = 0;
    if (offer_swap) {
        kinds[kind_count].label = "Swap (extra RAM)";
        kinds[kind_count].value = "swap";
        kind_count++;
    }
    kinds[kind_count].label = "LittleFS volume";
    kinds[kind_count].value = "littlefs";
    kind_count++;
    kinds[kind_count].label = "Back";
    kinds[kind_count].value = "back";
    kind_count++;

    size_t selected = 0;
    if (dialog__choice("New partition", "Type", kinds, kind_count, &selected) != BRUCE_OK) return;
    if (selected == kind_count - 1) return;

    char label[BRUCE_PARTITION_LABEL_MAX];
    bruce_partition_kind_t kind;
    if (offer_swap && selected == 0) {
        kind = BRUCE_PARTITION_KIND_SWAP;
        snprintf(label, sizeof(label), "%s", BRUCE_PARTITION_SWAP_LABEL);
    } else {
        kind = BRUCE_PARTITION_KIND_LITTLEFS;
        label[0] = '\0';
        /* Left to stage_create() to validate: one rule, in one place, and
         * bparted_common__error_text() already spells it out on rejection. */
        if (dialog__text_input("New partition", "Label", "", false, label, sizeof(label)) != BRUCE_OK) return;
    }

    uint64_t size_bytes = 0;
    if (!bparted_gui__pick_size(page->status.max_new_size, &size_bytes)) return;

    bruce_result_t result = partition_manager__stage_create(label, kind, size_bytes);
    if (result != BRUCE_OK) {
        bparted_gui__report("Create", result);
        return;
    }
    bparted_gui__notify(BRUCE_DIALOG_SUCCESS, "Added to the next-boot layout. Press Apply to save it.");
}

/* Picks a partition out of the next-boot layout for Delete or Format,
 * listing only the ones that verb can actually act on: Delete skips the
 * root entry (it can only be reformatted) and Format skips entries that do
 * not exist on flash yet (a new partition is formatted when it is created).
 * Rows already staged for deletion are gone from that layout and so are
 * skipped by both. */
static bool
bparted_gui__pick_target(const bparted_gui_page_t *page, bool for_format, char *out_label, size_t out_size) {
    /* Bounded by one layout, not by the planned list: everything skipped
     * below is exactly what makes the planned list the longer of the two. */
    char labels[BPARTED_LAYOUT_MAX][BPARTED_GUI_ROW_TEXT];
    bruce_dialog_choice_t choices[BPARTED_LAYOUT_MAX + 1];
    const bruce_partition_entry_t *targets[BPARTED_LAYOUT_MAX];
    size_t count = 0;

    for (size_t i = 0; i < page->planned_count && count < BPARTED_LAYOUT_MAX; ++i) {
        const bruce_partition_entry_t *entry = &page->planned[i];
        if (entry->state == BRUCE_PARTITION_STATE_DELETED) continue;
        if (for_format ? entry->state == BRUCE_PARTITION_STATE_NEW : entry->is_root) continue;

        bparted_gui__format_row(entry, labels[count], sizeof(labels[count]));
        choices[count].label = labels[count];
        choices[count].value = labels[count];
        targets[count] = entry;
        count++;
    }

    if (count == 0) {
        bparted_gui__notify(
            BRUCE_DIALOG_INFO,
            for_format ? "Nothing here can be formatted."
                       : "Nothing here can be deleted - '/' can only be formatted."
        );
        return false;
    }

    choices[count].label = "Back";
    choices[count].value = "back";
    count++;

    size_t selected = 0;
    if (dialog__choice(for_format ? "Format" : "Delete", "Which partition?", choices, count, &selected) !=
        BRUCE_OK) {
        return false;
    }
    if (selected == count - 1) return false;
    snprintf(out_label, out_size, "%s", targets[selected]->label);
    return true;
}

static void bparted_gui__delete(const bparted_gui_page_t *page) {
    char label[BRUCE_PARTITION_LABEL_MAX];
    if (!bparted_gui__pick_target(page, false, label, sizeof(label))) return;

    char message[128];
    snprintf(message, sizeof(message), "Delete '%s'? Everything on it is erased on the next boot.", label);
    if (!bparted_gui__confirm("Delete", message, "Delete")) return;

    bruce_result_t result = partition_manager__stage_delete(label);
    if (result != BRUCE_OK) {
        bparted_gui__report("Delete", result);
        return;
    }
    bparted_gui__notify(BRUCE_DIALOG_SUCCESS, "Removed from the next-boot layout. Press Apply to save it.");
}

static void bparted_gui__format(const bparted_gui_page_t *page) {
    char label[BRUCE_PARTITION_LABEL_MAX];
    if (!bparted_gui__pick_target(page, true, label, sizeof(label))) return;

    char message[128];
    snprintf(
        message, sizeof(message), "Erase and reformat '%s'? Everything on it is lost on the next boot.", label
    );
    if (!bparted_gui__confirm("Format", message, "Format")) return;

    bruce_result_t result = partition_manager__stage_format(label);
    if (result != BRUCE_OK) {
        bparted_gui__report("Format", result);
        return;
    }
    bparted_gui__notify(BRUCE_DIALOG_SUCCESS, "Marked for format. Press Apply to save it.");
}

/* The only way out of the app, reached by the Exit row and by the physical
 * Back/ESC button alike. Walking away from an edit that was never applied
 * silently loses it, and walking away from an applied one hides the fact
 * that it has not happened yet - so both get a prompt offering to finish
 * the job. A layout with nothing outstanding leaves with no prompt at all.
 * Returns true once the user has actually chosen to leave. */
static bool bparted_gui__confirm_exit(const bparted_gui_page_t *page) {
    if (page->status.has_pending_changes) {
        const bruce_dialog_choice_t choices[] = {
            {.label = "Apply and exit",   .value = "apply"  },
            {.label = "Discard and exit", .value = "discard"},
            {.label = "Stay",             .value = "stay"   },
        };
        size_t selected = 2;
        /* Backing out of the warning itself is another way of saying "stay". */
        if (dialog__choice(
                "Unapplied changes",
                "The next-boot layout has changes you have not applied.",
                choices,
                3,
                &selected
            ) != BRUCE_OK) {
            return false;
        }
        if (selected == 0) {
            /* Choosing "Apply and exit" is itself the confirmation, so this
             * skips the extra prompt bparted_gui__apply() would show. */
            if (!bparted_gui__commit()) return false;
            bparted_gui__reboot();
            return true;
        }
        if (selected == 1) {
            bruce_result_t result = partition_manager__discard();
            if (result != BRUCE_OK) {
                bparted_gui__report("Discard", result);
                return false;
            }
            return true;
        }
        return false;
    }

    if (page->status.reboot_required) {
        const bruce_dialog_choice_t choices[] = {
            {.label = "Reboot now",  .value = "reboot"},
            {.label = "Exit anyway", .value = "exit"  },
            {.label = "Stay",        .value = "stay"  },
        };
        size_t selected = 2;
        if (dialog__choice(
                "Not applied yet", "The saved layout only takes effect after a reboot.", choices, 3, &selected
            ) != BRUCE_OK) {
            return false;
        }
        if (selected == 0) {
            bparted_gui__reboot();
            return false;
        }
        return selected == 1;
    }

    return true;
}

int bparted_gui__main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    bparted_gui_page_t page;
    size_t selected = 0;
    for (;;) {
        bruce_result_t result = bparted_gui__refresh(&page);
        if (result != BRUCE_OK) {
            bparted_gui__report("Partitions", result);
            return result;
        }
        /* Keeps the cursor where it was across a redraw, so repeated edits
         * do not send it back to the top of the layout every time. */
        if (selected >= page.row_count) selected = 0;

        result = dialog__choice_launcher("Partitions", page.summary, page.choices, page.row_count, &selected);
        if (result != BRUCE_OK && result != BRUCE_ERR_CANCELLED) {
            bparted_gui__report("Partitions", result);
            return result;
        }

        /* Back/ESC means the same thing as the Exit row, warning included. */
        bparted_gui_action_t action =
            result == BRUCE_OK ? (bparted_gui_action_t)page.actions[selected] : BPARTED_GUI_ACTION_EXIT;
        switch (action) {
            case BPARTED_GUI_ACTION_CREATE: bparted_gui__create(&page); break;
            case BPARTED_GUI_ACTION_DELETE: bparted_gui__delete(&page); break;
            case BPARTED_GUI_ACTION_FORMAT: bparted_gui__format(&page); break;
            case BPARTED_GUI_ACTION_APPLY: bparted_gui__apply(&page); break;
            case BPARTED_GUI_ACTION_CANCEL: bparted_gui__cancel(&page); break;
            case BPARTED_GUI_ACTION_REBOOT: bparted_gui__reboot(); break;
            case BPARTED_GUI_ACTION_EXIT:
                if (bparted_gui__confirm_exit(&page)) return 0;
                break;
            case BPARTED_GUI_ACTION_NONE: break;
        }
    }
}
