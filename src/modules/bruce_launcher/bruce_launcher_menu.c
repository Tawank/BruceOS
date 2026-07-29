#include "bruce_launcher_menu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core_sdk/manifest.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"

#define BRUCE_LAUNCHER_CONFIG_PATH "/launcher.json"
#define BRUCE_LAUNCHER_JSON_MAX 8192
#define BRUCE_LAUNCHER_MAX_DEPTH 4
#define BRUCE_LAUNCHER_TITLE "Main Menu"

typedef struct {
    uint8_t *next;
    size_t remaining;
} bruce_launcher_menu_arena_t;

/* Default launcher configuration written when /launcher.json is missing. */
static const char *BRUCE_LAUNCHER_DEFAULT_JSON =
    "{\n"
    "  \"WiFi@wifi\": {\n"
    "    \"Connect to Wifi\": \"wifi connect\",\n"
    "    \"Start WiFi AP\": \"wifi ap start --gui\",\n"
    "    \"Turn Off WiFi\": \"wifi disconnect\",\n"
    "    \"AP info\": \"wifi ap info --gui\",\n"
    "    \"WebUI\": \"webui --gui\",\n"
    "    \"Wifi Atks\": {\n"
    "      \"Target Atks\": \"wifiatks target --gui\",\n"
    "      \"Karma Attack\": \"wifiatks karma --gui\",\n"
    "      \"Beacon SPAM\": \"wifiatks beacon --gui\"\n"
    "    }\n"
    "  },\n"
    "  \"Bluetooth@bluetooth\": \"bluetooth --gui\",\n"
    "  \"Infrared@infrared\": \"ir --gui\",\n"
    "  \"NRF24@radio-handheld\": \"nrf24 --gui\",\n"
    "  \"Files@folder\": \"filemanager --gui\",\n"
    "  \"Terminal@console\": \"terminal --gui\",\n"
    "  \"Clock@clock-outline\": \"clock --gui\",\n"
    "  \"Config@cog\": {\n"
    "    \"Display & UI\": \"config display --gui\",\n"
    "    \"LED Config\": \"config led --gui\",\n"
    "    \"Audio Config\": \"config audio --gui\",\n"
    "    \"System Config\": {"
    "      \"Insta boot\": \"config system instant_boot toggle --gui\",\n"
    "      \"Wifi Startup\": \"config system wifi_startup toggle --gui\",\n"
    "      \"Startup Apps\": \"config system startup_apps --gui\",\n"
    "      \"Clock\": \"config system clock --gui\",\n"
    "      \"Advanced\": {\n"
    "        \"Factory reset\": \"config system reset_defaults confirm --gui\"\n"
    "      }\n"
    "    },\n"
    "    \"Power\": \"config power --gui\",\n"
    "    \"Install App Store\": \"appstore install --gui\",\n"
    "    \"About\": \"config about --gui\"\n"
    "  },\n"
    "  \"Selftest@test-tube\": \"terminal selftest --gui\",\n"
    "  \"Apps@apps\": \"apps --gui\"\n"
    "}\n";

static void bruce_launcher__parse_label(
    const char *source, char *label, size_t label_size, char *icon_name, size_t icon_name_size
) {
    const char *separator = strrchr(source, '@');
    size_t label_length = separator != NULL ? (size_t)(separator - source) : strlen(source);
    if (label_length >= label_size) label_length = label_size - 1;
    memcpy(label, source, label_length);
    label[label_length] = '\0';

    if (icon_name == NULL || icon_name_size == 0) return;
    icon_name[0] = '\0';
    if (separator == NULL || separator[1] == '\0') return;
    strncpy(icon_name, separator + 1, icon_name_size - 1);
    icon_name[icon_name_size - 1] = '\0';
}

static bruce_launcher_menu_t *bruce_launcher__menu_create(
    bruce_launcher_menu_arena_t *arena, const char *title, bruce_launcher_menu_t *parent, int capacity
) {
    size_t entries_size = (size_t)capacity * sizeof(bruce_launcher_entry_t);
    size_t allocation_size = sizeof(bruce_launcher_menu_t) + entries_size;
    if (arena == NULL || allocation_size > arena->remaining) return NULL;

    bruce_launcher_menu_t *menu = (bruce_launcher_menu_t *)arena->next;
    arena->next += allocation_size;
    arena->remaining -= allocation_size;

    bruce_launcher__parse_label(title, menu->title, sizeof(menu->title), NULL, 0);
    menu->capacity = capacity;
    menu->entries = capacity > 0 ? (bruce_launcher_entry_t *)(menu + 1) : NULL;
    menu->parent = parent;
    return menu;
}

void bruce_launcher__menu_free(bruce_launcher_menu_t *menu) { memory__free(menu); }

static bool
bruce_launcher__menu_add_command(bruce_launcher_menu_t *menu, const char *label, const char *command) {
    if (menu->entry_count >= menu->capacity) return false;
    bruce_launcher_entry_t *entry = &menu->entries[menu->entry_count++];
    bruce_launcher__parse_label(
        label, entry->label, sizeof(entry->label), entry->icon_name, sizeof(entry->icon_name)
    );
    strncpy(entry->command, command, sizeof(entry->command) - 1);
    entry->kind = BRUCE_LAUNCHER_ENTRY_COMMAND;
    return true;
}

static bool bruce_launcher__menu_add_submenu(
    bruce_launcher_menu_t *menu, const char *label, bruce_launcher_menu_t *submenu
) {
    if (menu->entry_count >= menu->capacity) return false;
    bruce_launcher_entry_t *entry = &menu->entries[menu->entry_count++];
    bruce_launcher__parse_label(
        label, entry->label, sizeof(entry->label), entry->icon_name, sizeof(entry->icon_name)
    );
    entry->submenu = submenu;
    entry->kind = BRUCE_LAUNCHER_ENTRY_SUBMENU;
    return true;
}

static bool bruce_launcher__menu_add_back(bruce_launcher_menu_t *menu) {
    if (menu->entry_count >= menu->capacity) return false;
    bruce_launcher_entry_t *entry = &menu->entries[menu->entry_count++];
    strncpy(entry->label, "Back", sizeof(entry->label) - 1);
    entry->kind = BRUCE_LAUNCHER_ENTRY_BACK;
    return true;
}

static char *bruce_launcher__read_file(const char *path) {
    bruce_file_id_t file;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return NULL;

    char *buffer = (char *)memory__malloc(BRUCE_LAUNCHER_JSON_MAX);
    if (buffer == NULL) {
        storage__close(file);
        return NULL;
    }

    size_t total = 0;
    for (;;) {
        size_t chunk = 0;
        bruce_result_t result =
            storage__read(file, buffer + total, BRUCE_LAUNCHER_JSON_MAX - total - 1, &chunk);
        if (result != BRUCE_OK || chunk == 0) break;
        total += chunk;
        if (total >= BRUCE_LAUNCHER_JSON_MAX - 1) break;
    }
    buffer[total] = '\0';
    storage__close(file);
    return buffer;
}

static bool bruce_launcher__write_default_config(void) {
    bruce_file_id_t file;
    bruce_result_t result = storage__open(
        BRUCE_LAUNCHER_CONFIG_PATH,
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &file
    );
    if (result != BRUCE_OK) return false;

    size_t length = strlen(BRUCE_LAUNCHER_DEFAULT_JSON);
    size_t written = 0;
    result = storage__write(file, BRUCE_LAUNCHER_DEFAULT_JSON, length, &written);
    storage__close(file);
    return result == BRUCE_OK && written == length;
}

static int bruce_launcher__discover_apps(bruce_launcher_menu_t *menu, const char *path) {
    bruce_storage_entry_t *entries =
        (bruce_storage_entry_t *)memory__malloc(sizeof(*entries) * BRUCE_LAUNCHER_MAX_ENTRIES);
    if (entries == NULL) return 0;

    size_t count = 0;
    if (storage__list(path, entries, BRUCE_LAUNCHER_MAX_ENTRIES, &count) != BRUCE_OK) {
        memory__free(entries);
        return 0;
    }

    int added = 0;
    for (size_t i = 0; i < count && (menu == NULL || menu->entry_count + 1 < menu->capacity); ++i) {
        if (entries[i].type != BRUCE_STORAGE_ENTRY_FILE) continue;

        char full_path[BRUCE_STORAGE_PATH_MAX];
        int printed = snprintf(full_path, sizeof(full_path), "%s/%s", path, entries[i].name);
        if (printed < 0 || (size_t)printed >= sizeof(full_path)) continue;

        const char *json = manifest__inspect_path(full_path);
        if (json == NULL) continue;
        bruce_manifest_t *manifest = manifest__parse(json, strlen(json));
        free((void *)json);
        if (manifest == NULL) continue;

        if (menu == NULL || bruce_launcher__menu_add_command(menu, manifest->app_name, full_path)) {
            added++;
        }
        memory__free(manifest);
    }

    memory__free(entries);
    return added;
}

static int bruce_launcher__discovered_menu_capacity(const char *path) {
    int capacity = bruce_launcher__discover_apps(NULL, path);
    if (capacity >= BRUCE_LAUNCHER_MAX_ENTRIES) capacity = BRUCE_LAUNCHER_MAX_ENTRIES - 1;
    return capacity + 1;
}

static bruce_launcher_menu_t *bruce_launcher__parse_json_object(
    bruce_launcher_menu_arena_t *arena, cJSON *root, const char *title, bruce_launcher_menu_t *parent,
    int depth
);

static int bruce_launcher__json_entry_count(cJSON *root, bool include_back, int depth) {
    int count = include_back ? 1 : 0;
    cJSON *child;
    cJSON_ArrayForEach(child, root) {
        bool is_command = cJSON_IsString(child) && child->valuestring != NULL;
        bool is_submenu = cJSON_IsObject(child) && depth < BRUCE_LAUNCHER_MAX_DEPTH;
        if (child->string != NULL && (is_command || is_submenu)) count++;
        if (count >= BRUCE_LAUNCHER_MAX_ENTRIES) break;
    }
    return count;
}

static size_t bruce_launcher__json_allocation_size(cJSON *root, bool include_back, int depth) {
    int capacity = bruce_launcher__json_entry_count(root, include_back, depth);
    size_t size = sizeof(bruce_launcher_menu_t) + (size_t)capacity * sizeof(bruce_launcher_entry_t);
    int child_limit = capacity - (include_back ? 1 : 0);
    int child_count = 0;

    cJSON *child;
    cJSON_ArrayForEach(child, root) {
        bool is_command = cJSON_IsString(child) && child->valuestring != NULL;
        bool is_submenu = cJSON_IsObject(child) && depth < BRUCE_LAUNCHER_MAX_DEPTH;
        if (child->string == NULL || (!is_command && !is_submenu)) continue;
        if (child_count >= child_limit) break;
        child_count++;

        size_t child_size = 0;
        if (is_command && child->valuestring[0] == '/') {
            int capacity = bruce_launcher__discovered_menu_capacity(child->valuestring);
            /* Carry the preflight result on the temporary JSON node into arena construction. */
            child->valueint = capacity;
            child_size = sizeof(bruce_launcher_menu_t) + (size_t)capacity * sizeof(bruce_launcher_entry_t);
        } else if (is_submenu) {
            child_size = bruce_launcher__json_allocation_size(child, true, depth + 1);
        }
        if (child_size > SIZE_MAX - size) return 0;
        size += child_size;
    }
    return size;
}

static bool bruce_launcher__parse_json_value(
    bruce_launcher_menu_arena_t *arena, bruce_launcher_menu_t *menu, const char *key, cJSON *value, int depth
) {
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        if (value->valuestring[0] != '/') {
            return bruce_launcher__menu_add_command(menu, key, value->valuestring);
        }
        bruce_launcher_menu_t *submenu = bruce_launcher__menu_create(arena, key, menu, value->valueint);
        if (submenu == NULL) return false;
        (void)bruce_launcher__discover_apps(submenu, value->valuestring);
        (void)bruce_launcher__menu_add_back(submenu);
        if (bruce_launcher__menu_add_submenu(menu, key, submenu)) return true;
        return false;
    }

    if (cJSON_IsObject(value) && depth < BRUCE_LAUNCHER_MAX_DEPTH) {
        bruce_launcher_menu_t *submenu =
            bruce_launcher__parse_json_object(arena, value, key, menu, depth + 1);
        if (submenu == NULL) return false;
        if (bruce_launcher__menu_add_submenu(menu, key, submenu)) return true;
        return false;
    }
    return true;
}

static bruce_launcher_menu_t *bruce_launcher__parse_json_object(
    bruce_launcher_menu_arena_t *arena, cJSON *root, const char *title, bruce_launcher_menu_t *parent,
    int depth
) {
    int capacity = bruce_launcher__json_entry_count(root, parent != NULL, depth);
    bruce_launcher_menu_t *menu = bruce_launcher__menu_create(arena, title, parent, capacity);
    if (menu == NULL) return NULL;

    cJSON *child;
    cJSON_ArrayForEach(child, root) {
        if (child->string != NULL &&
            !bruce_launcher__parse_json_value(arena, menu, child->string, child, depth))
            break;
    }
    if (parent != NULL) (void)bruce_launcher__menu_add_back(menu);
    return menu;
}

static bruce_launcher_menu_t *bruce_launcher__parse_json(cJSON *root) {
    size_t allocation_size = bruce_launcher__json_allocation_size(root, false, 0);
    if (allocation_size == 0) return NULL;

    void *allocation = memory__calloc(1, allocation_size);
    if (allocation == NULL) return NULL;
    bruce_launcher_menu_arena_t arena = {
        .next = (uint8_t *)allocation,
        .remaining = allocation_size,
    };
    bruce_launcher_menu_t *menu =
        bruce_launcher__parse_json_object(&arena, root, BRUCE_LAUNCHER_TITLE, NULL, 0);
    if (menu == NULL) memory__free(allocation);
    return menu;
}

bruce_launcher_menu_t *bruce_launcher__menu_load(void) {
    char *text = bruce_launcher__read_file(BRUCE_LAUNCHER_CONFIG_PATH);
    if (text == NULL) {
        (void)bruce_launcher__write_default_config();
        text = bruce_launcher__read_file(BRUCE_LAUNCHER_CONFIG_PATH);
    }

    cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
    memory__free(text);
    if (root != NULL && cJSON_IsObject(root)) {
        bruce_launcher_menu_t *menu = bruce_launcher__parse_json(root);
        cJSON_Delete(root);
        if (menu != NULL) return menu;
    } else if (root != NULL) {
        cJSON_Delete(root);
    }

    root = cJSON_Parse(BRUCE_LAUNCHER_DEFAULT_JSON);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    bruce_launcher_menu_t *menu = bruce_launcher__parse_json(root);
    cJSON_Delete(root);
    return menu;
}
