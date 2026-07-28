#include "bruce_launcher_icons.h"

#include "core_sdk/display.h"
#include "core_sdk/icon.h"

void bruce_launcher__draw_entry_icon(
    const bruce_launcher_entry_t *entry, int cx, int cy, int size, uint16_t color
) {
    if (entry == NULL || entry->icon_name[0] == '\0') return;
    const bruce_icon_t *icon = icon__get(entry->icon_name);
    if (icon == NULL) return;
    display__draw_bitmap_scaled(
        (int16_t)(cx - size / 2),
        (int16_t)(cy - size / 2),
        icon->bits,
        icon->width,
        icon->height,
        (int16_t)size,
        (int16_t)size,
        color
    );
}
