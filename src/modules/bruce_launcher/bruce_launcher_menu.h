#pragma once

#include <stdbool.h>

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
    char command[BRUCE_LAUNCHER_COMMAND_MAX];
    bruce_launcher_menu_t *submenu;
} bruce_launcher_entry_t;

struct bruce_launcher_menu {
    char title[BRUCE_LAUNCHER_LABEL_MAX];
    bruce_launcher_entry_t *entries;
    int entry_count;
    int capacity;
    bruce_launcher_menu_t *parent;
};

bruce_launcher_menu_t *bruce_launcher__menu_load(void);
void bruce_launcher__menu_free(bruce_launcher_menu_t *menu);
