#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BRUCE_LAUNCHER_MAX_ENTRIES 32
#define BRUCE_LAUNCHER_LABEL_MAX 48
#define BRUCE_LAUNCHER_ICON_NAME_MAX 32
#define BRUCE_LAUNCHER_COMMAND_MAX 128

typedef struct bruce_launcher_menu bruce_launcher_menu_t;

typedef enum {
    BRUCE_LAUNCHER_ENTRY_COMMAND,
    BRUCE_LAUNCHER_ENTRY_SUBMENU,
    BRUCE_LAUNCHER_ENTRY_BACK,
} bruce_launcher_entry_kind_t;

typedef struct {
    char label[BRUCE_LAUNCHER_LABEL_MAX];
    char icon_name[BRUCE_LAUNCHER_ICON_NAME_MAX];
    bruce_launcher_entry_kind_t kind;
    union {
        char command[BRUCE_LAUNCHER_COMMAND_MAX];
        /* Byte offset from this entry's own menu to its submenu. The whole
         * tree lives in one memory__external block addressed only through a
         * read-only mapping (see bruce_launcher_menu.c), so submenus can't be
         * linked with live pointers baked in at build time; a parent-relative
         * offset stays valid no matter where the block ends up mapped. */
        uint32_t submenu_offset;
    };
} bruce_launcher_entry_t;

struct bruce_launcher_menu {
    char title[BRUCE_LAUNCHER_LABEL_MAX];
    int entry_count;
    int capacity;
    bool is_root;
};

bruce_launcher_menu_t *bruce_launcher__menu_load(void);
void bruce_launcher__menu_free(bruce_launcher_menu_t *menu);

/* Entries always sit immediately after their menu header in the backing
 * memory__external block. */
const bruce_launcher_entry_t *bruce_launcher__menu_entries(const bruce_launcher_menu_t *menu);

/* The text to display for `entry`. Usually entry->label verbatim, but entries
 * whose action toggles carry a "$NAME" placeholder that resolves against live
 * state (e.g. "$WIFI_CONNECT_TEXT" -> "Connect WiFi"/"Disconnect WiFi"), so
 * call this every time the label is drawn rather than caching the result.
 * The returned pointer is either into `entry` or a string literal. */
const char *bruce_launcher__entry_label(const bruce_launcher_entry_t *entry);

/* Resolves an entry's submenu from its parent-relative offset. Returns NULL
 * if entry is not a submenu entry. */
const bruce_launcher_menu_t *
bruce_launcher__entry_submenu(const bruce_launcher_menu_t *menu, const bruce_launcher_entry_t *entry);
