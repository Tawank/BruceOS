#pragma once

#include <stdint.h>

#include "bruce_launcher_menu.h"

void bruce_launcher__draw_entry_icon(
    const bruce_launcher_entry_t *entry, int cx, int cy, int size, uint16_t color
);
