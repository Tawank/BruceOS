#include <stddef.h>
#include <string.h>

#include "icon_assets.h"

const bruce_icon_t *icon__get(const char *name) {
    if (name == NULL) { return NULL; }
    for (size_t i = 0; i < (sizeof(s_icons) / sizeof(s_icons[0])); ++i) {
        if (strcmp(name, s_icons[i].name) == 0) { return &s_icons[i].icon; }
    }
    return NULL;
}
