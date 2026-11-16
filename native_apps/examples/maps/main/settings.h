#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t lat_e6;
    int32_t lon_e6;
    int zoom;
    bool online;
    char tile_dir[96];
    char tile_url[128];
    char search_url[128];
} settings_t;

void settings__load(settings_t *settings);
void settings__save_view(const settings_t *settings);
void settings__save_online(bool online);
