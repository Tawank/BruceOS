#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/dialog.h"
#include "core_sdk/icon.h"
#include "core_sdk/memory.h"
#include "icon_assets.h"

const bruce_icon_t *icon__get(const char *name) {
    if (name == NULL) { return NULL; }
    for (size_t i = 0; i < (sizeof(s_icons) / sizeof(s_icons[0])); ++i) {
        if (strcmp(name, s_icons[i].name) == 0) { return &s_icons[i].icon; }
    }
    return NULL;
}

bruce_result_t icon__pick(
    const char *title, const char *current_icon_name, bool allow_none, char *out_icon_name,
    size_t out_icon_name_size
) {
    if (out_icon_name == NULL || out_icon_name_size == 0) { return BRUCE_ERR_INVALID_ARGUMENT; }

    size_t icon_count = sizeof(s_icons) / sizeof(s_icons[0]);
    size_t count = icon_count + (allow_none ? 1 : 0);
    bruce_dialog_choice_t *choices = memory__calloc(count, sizeof(*choices));
    if (choices == NULL) { return BRUCE_ERR_NO_MEMORY; }

    size_t n = 0;
    size_t selected = 0;
    if (allow_none) {
        choices[n] = (bruce_dialog_choice_t){.label = "None", .value = "none"};
        if (current_icon_name == NULL || current_icon_name[0] == '\0') { selected = n; }
        n++;
    }
    for (size_t i = 0; i < icon_count; ++i, ++n) {
        choices[n] = (bruce_dialog_choice_t){
            .label = s_icons[i].name, .value = s_icons[i].name, .icon_name = s_icons[i].name
        };
        if (current_icon_name != NULL && strcmp(current_icon_name, s_icons[i].name) == 0) { selected = n; }
    }

    bruce_result_t result = dialog__choice(title, NULL, choices, count, &selected);
    if (result == BRUCE_OK) {
        if (allow_none && strcmp(choices[selected].value, "none") == 0) { out_icon_name[0] = '\0'; }
        else { snprintf(out_icon_name, out_icon_name_size, "%s", choices[selected].value); }
    }
    memory__free(choices);
    return result;
}
