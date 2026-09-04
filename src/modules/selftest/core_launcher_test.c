/* Pure-logic unit coverage for core/launcher/launcher.c's entry-key/label
 * building and menu-lookup helpers -- no storage I/O (launcher__add_menu_entry()/
 * launcher__menu_has_command()/launcher__list_menus() themselves read and
 * write /config/launcher.conf, which selftest doesn't exercise here). See
 * core/launcher/launcher_internal.h for why these are reachable from here. */
#include "core_launcher_test.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "core/launcher/launcher_internal.h"
#include "core_sdk/launcher.h"

/* ------------------------------------------------------------------------ */
/* selftest__run_core_launcher_build_entry_key_case                         */
/* ------------------------------------------------------------------------ */

bool selftest__run_core_launcher_build_entry_key_case(void) {
    char key[BRUCE_LAUNCHER_ENTRY_LABEL_MAX + 1 + BRUCE_LAUNCHER_ENTRY_ICON_MAX];

    bool with_icon_ok =
        launcher__build_entry_key("NES", "remote-tv", key, sizeof(key)) && strcmp(key, "NES@remote-tv") == 0;

    /* No icon (NULL, or "") yields the bare label -- no trailing "@", which
     * launcher__label_from_key() (and bruce_launcher__parse_label() in
     * modules/bruce_launcher/bruce_launcher_menu.c) requires to leave an
     * entry's icon_name empty. */
    bool no_icon_null_ok = launcher__build_entry_key("NES", NULL, key, sizeof(key)) && strcmp(key, "NES") == 0;
    bool no_icon_empty_ok = launcher__build_entry_key("NES", "", key, sizeof(key)) && strcmp(key, "NES") == 0;

    /* A NULL/empty label, or a zero-capacity buffer, is rejected rather than
     * silently producing a garbage/empty key. */
    bool null_label_rejected = !launcher__build_entry_key(NULL, "cog", key, sizeof(key));
    bool empty_label_rejected = !launcher__build_entry_key("", "cog", key, sizeof(key));
    bool zero_capacity_rejected = !launcher__build_entry_key("NES", "cog", key, 0);

    bool ok = with_icon_ok && no_icon_null_ok && no_icon_empty_ok && null_label_rejected &&
              empty_label_rejected && zero_capacity_rejected;
    printf(
        "[selftest] core/launcher-build-entry-key: %s (with_icon=%d no_icon_null=%d no_icon_empty=%d "
        "null_label_rejected=%d empty_label_rejected=%d zero_capacity_rejected=%d)\n",
        ok ? "OK" : "FAIL", with_icon_ok, no_icon_null_ok, no_icon_empty_ok, null_label_rejected,
        empty_label_rejected, zero_capacity_rejected
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_core_launcher_label_from_key_case                          */
/* ------------------------------------------------------------------------ */

bool selftest__run_core_launcher_label_from_key_case(void) {
    char label[BRUCE_LAUNCHER_ENTRY_LABEL_MAX];

    /* Round-trips launcher__build_entry_key()'s own output. */
    bool with_icon_ok = launcher__label_from_key("NES@remote-tv", label, sizeof(label)) &&
                        strcmp(label, "NES") == 0;
    bool no_icon_ok = launcher__label_from_key("NES", label, sizeof(label)) && strcmp(label, "NES") == 0;

    /* A truncated destination buffer still gets a NUL-terminated result. */
    char small[3];
    bool truncated_ok = launcher__label_from_key("WiFi Atks@wifi", small, sizeof(small)) &&
                        strcmp(small, "Wi") == 0;

    bool null_key_rejected = !launcher__label_from_key(NULL, label, sizeof(label));
    bool zero_capacity_rejected = !launcher__label_from_key("WiFi", label, 0);

    bool ok = with_icon_ok && no_icon_ok && truncated_ok && null_key_rejected && zero_capacity_rejected;
    printf(
        "[selftest] core/launcher-label-from-key: %s (with_icon=%d no_icon=%d truncated=%d "
        "null_key_rejected=%d zero_capacity_rejected=%d)\n",
        ok ? "OK" : "FAIL", with_icon_ok, no_icon_ok, truncated_ok, null_key_rejected, zero_capacity_rejected
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_core_launcher_json_has_command_case                        */
/* ------------------------------------------------------------------------ */

bool selftest__run_core_launcher_json_has_command_case(void) {
    cJSON *root = cJSON_Parse(
        "{\"Files@folder-open\": \"filemanager\", \"Apps@apps\": \"apps\", "
        "\"Config@cog\": {\"BLE scan@bluetooth\": \"bluetooth scan\"}}"
    );

    bool hit_ok = launcher__json_has_command(root, "filemanager");

    /* A command nested inside a submenu object doesn't count -- this only
     * looks at flat entries directly on the given menu object, matching
     * what launcher__add_menu_entry() appends to. */
    bool nested_rejected = !launcher__json_has_command(root, "bluetooth scan");
    bool miss_rejected = !launcher__json_has_command(root, "clock");

    bool null_menu_rejected = !launcher__json_has_command(NULL, "filemanager");
    bool null_command_rejected = !launcher__json_has_command(root, NULL);

    cJSON_Delete(root);

    bool ok = hit_ok && nested_rejected && miss_rejected && null_menu_rejected && null_command_rejected;
    printf(
        "[selftest] core/launcher-json-has-command: %s (hit=%d nested_rejected=%d miss_rejected=%d "
        "null_menu_rejected=%d null_command_rejected=%d)\n",
        ok ? "OK" : "FAIL", hit_ok, nested_rejected, miss_rejected, null_menu_rejected, null_command_rejected
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_core_launcher_json_menu_labels_case                        */
/* ------------------------------------------------------------------------ */

bool selftest__run_core_launcher_json_menu_labels_case(void) {
    cJSON *root = cJSON_Parse(
        "{\"Files@folder-open\": \"filemanager\", \"WiFi@wifi\": {\"Scan\": \"wifi scan\"}, "
        "\"Bluetooth@bluetooth\": {\"BLE scan@bluetooth\": \"bluetooth scan\"}}"
    );

    char labels[8][BRUCE_LAUNCHER_ENTRY_LABEL_MAX];
    size_t count = launcher__json_menu_labels(root, labels, 8);
    /* Root always comes first, then only the two submenus -- "Files", a
     * flat command entry, is skipped. */
    bool root_first_ok = count == 3 && strcmp(labels[0], BRUCE_LAUNCHER_ROOT_MENU_LABEL) == 0;
    bool wifi_found = false, bluetooth_found = false;
    for (size_t i = 1; i < count; ++i) {
        if (strcmp(labels[i], "WiFi") == 0) wifi_found = true;
        if (strcmp(labels[i], "Bluetooth") == 0) bluetooth_found = true;
    }

    /* A NULL root (config missing/unparsable) still yields the root label
     * alone, never zero. */
    size_t null_root_count = launcher__json_menu_labels(NULL, labels, 8);
    bool null_root_ok = null_root_count == 1 && strcmp(labels[0], BRUCE_LAUNCHER_ROOT_MENU_LABEL) == 0;

    /* capacity=1 caps the list at just the root label, even though root has submenus. */
    size_t capped_count = launcher__json_menu_labels(root, labels, 1);
    bool capped_ok = capped_count == 1;

    cJSON_Delete(root);

    bool ok = root_first_ok && wifi_found && bluetooth_found && null_root_ok && capped_ok;
    printf(
        "[selftest] core/launcher-json-menu-labels: %s (root_first=%d wifi=%d bluetooth=%d null_root=%d "
        "capped=%d)\n",
        ok ? "OK" : "FAIL", root_first_ok, wifi_found, bluetooth_found, null_root_ok, capped_ok
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_core_launcher_json_find_menu_case                          */
/* ------------------------------------------------------------------------ */

bool selftest__run_core_launcher_json_find_menu_case(void) {
    cJSON *root = cJSON_Parse(
        "{\"Files@folder-open\": \"filemanager\", \"WiFi@wifi\": {\"Scan\": \"wifi scan\"}}"
    );

    bool root_null_ok = launcher__json_find_menu(root, NULL) == root;
    bool root_empty_ok = launcher__json_find_menu(root, "") == root;
    bool root_label_ok = launcher__json_find_menu(root, BRUCE_LAUNCHER_ROOT_MENU_LABEL) == root;

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "WiFi@wifi");
    bool submenu_ok = launcher__json_find_menu(root, "WiFi") == wifi;

    /* "Files" names a flat command entry, not a submenu, so it isn't a
     * valid destination even though it's a root key. */
    bool flat_entry_rejected = launcher__json_find_menu(root, "Files") == NULL;
    bool unknown_rejected = launcher__json_find_menu(root, "Nope") == NULL;
    bool null_root_rejected = launcher__json_find_menu(NULL, "WiFi") == NULL;

    cJSON_Delete(root);

    bool ok = root_null_ok && root_empty_ok && root_label_ok && submenu_ok && flat_entry_rejected &&
              unknown_rejected && null_root_rejected;
    printf(
        "[selftest] core/launcher-json-find-menu: %s (root_null=%d root_empty=%d root_label=%d submenu=%d "
        "flat_entry_rejected=%d unknown_rejected=%d null_root_rejected=%d)\n",
        ok ? "OK" : "FAIL", root_null_ok, root_empty_ok, root_label_ok, submenu_ok, flat_entry_rejected,
        unknown_rejected, null_root_rejected
    );
    return ok;
}
