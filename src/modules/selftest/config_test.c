/* Config acceptance coverage: type-safe singleton getters/setters, `config`
 * permission enforcement, and the
 * permanently-protected field group
 * (wifiApSsid/wifiApPassword, wifiCredentials, wifiMAC) that stays denied to
 * an external process no matter what it has been granted.
 *
 * Like permission_test.c/storage_test.c, the "external app" processes these
 * tests need are created directly via the Core-private
 * process_registry__create() (no ELF/JS loader exists yet to launch a real
 * one). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config/config.h"
#include "core/dialog/dialog.h"
#include "core/permission/permission.h"
#include "core/process/process.h"
#include "core/storage/storage.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/process.h"
#include "modules/config/config_app.h"

#include "config_test.h"

/* ------------------------------------------------------------------------ */
/* Mock dialog__choice() provider (same pattern as permission_test.c)        */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile int call_count;
    volatile size_t next_selection;
} selftest__config_dialog_mock_t;

static selftest__config_dialog_mock_t s_mock;

static bruce_result_t selftest__config_dialog_mock_provider(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title;
    (void)message;
    (void)choices;
    (void)choice_count;
    s_mock.call_count++;
    *out_selected = s_mock.next_selection;
    return BRUCE_OK;
}

static void selftest__config_dialog_mock_reset(size_t selection) {
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.next_selection = selection;
    dialog__test_set_choice_provider(selftest__config_dialog_mock_provider);
}

static void selftest__config_dialog_mock_clear(void) { dialog__test_set_choice_provider(NULL); }

/* ------------------------------------------------------------------------ */
/* Helper: run a body from a chosen built_in/permission_key process context     */
/* ------------------------------------------------------------------------ */

typedef int (*selftest__config_entry_t)(int argc, char **argv);

static bool
selftest__config_run_as(bool built_in, const char *permission_key, selftest__config_entry_t entry) {
    process_create_params_t params = {
        .name = "selftest_config",
        .entry = entry,
        .argc = 0,
        .argv = NULL,
        .built_in = built_in,
        .gui_requested = false,
        .permission_key = permission_key,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) return false;
    bruce_result_t wait_result = process__wait(id, 2000);
    return wait_result == BRUCE_OK || wait_result == BRUCE_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_permission_denied_case                              */
/* ------------------------------------------------------------------------ */

static volatile bruce_result_t s_config_get_result;
static volatile bruce_result_t s_config_set_result;

static int selftest__config_bright_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_config_set_result = config__set_display_brightness(55);
    s_config_get_result = config__get_display_brightness() == 0 ? BRUCE_ERR_PERMISSION : BRUCE_OK;
    return 0;
}

bool selftest__run_config_permission_denied_case(void) {
    permission__test_reset();
    selftest__config_dialog_mock_reset(1 /* Deny */);
    s_config_get_result = BRUCE_OK;
    s_config_set_result = BRUCE_OK;

    bool ran = selftest__config_run_as(false, "selftest_config_denied.elf", selftest__config_bright_entry);

    selftest__config_dialog_mock_clear();

    bool ok =
        ran && s_config_set_result == BRUCE_ERR_PERMISSION && s_config_get_result == BRUCE_ERR_PERMISSION;
    printf(
        "[selftest] config/permission-denied: %s (set=%d get=%d)\n",
        ok ? "OK" : "FAIL",
        s_config_set_result,
        s_config_get_result
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_permission_allowed_case                             */
/* ------------------------------------------------------------------------ */

static volatile int s_config_bright_value;

static int selftest__config_bright_allowed_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_config_set_result = config__set_display_brightness(66);
    int value = config__get_display_brightness();
    s_config_get_result = value == 66 ? BRUCE_OK : BRUCE_ERR_INTERNAL;
    s_config_bright_value = value;
    return 0;
}

bool selftest__run_config_permission_allowed_case(void) {
    permission__test_reset();
    selftest__config_dialog_mock_reset(0 /* Allow */);
    s_config_get_result = BRUCE_ERR_INTERNAL;
    s_config_set_result = BRUCE_ERR_INTERNAL;
    s_config_bright_value = -1;

    bool ran =
        selftest__config_run_as(false, "selftest_config_allowed.elf", selftest__config_bright_allowed_entry);

    selftest__config_dialog_mock_clear();

    bool ok = ran && s_config_set_result == BRUCE_OK && s_config_get_result == BRUCE_OK &&
              s_config_bright_value == 66;
    printf(
        "[selftest] config/permission-allowed: %s (set=%d get=%d value=%d)\n",
        ok ? "OK" : "FAIL",
        s_config_set_result,
        s_config_get_result,
        s_config_bright_value
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_protected_field_denied_case                         */
/* ------------------------------------------------------------------------ */

static int selftest__config_protected_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_config_get_result = config__get_wifi_ap_ssid() == NULL ? BRUCE_ERR_PERMISSION : BRUCE_OK;
    s_config_set_result = config__set_wifi_mac("aa:bb:cc:dd:ee:ff");
    return 0;
}

bool selftest__run_config_protected_field_denied_case(void) {
    permission__test_reset();
    /* Mock answers Allow: even if the external process were to request and be
     * granted `config`, the permanently-protected group must still deny it -
     * these two calls never even consult permission__check()/the dialog. */
    selftest__config_dialog_mock_reset(0 /* Allow */);
    s_config_get_result = BRUCE_OK;
    s_config_set_result = BRUCE_OK;

    bool ran =
        selftest__config_run_as(false, "selftest_config_protected.elf", selftest__config_protected_entry);
    int calls = s_mock.call_count;

    selftest__config_dialog_mock_clear();

    bool ok = ran && s_config_get_result == BRUCE_ERR_PERMISSION &&
              s_config_set_result == BRUCE_ERR_PERMISSION && calls == 0;
    printf(
        "[selftest] config/protected-field-denied: %s (get=%d set=%d dialog_calls=%d)\n",
        ok ? "OK" : "FAIL",
        s_config_get_result,
        s_config_set_result,
        calls
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_builtin_manage_case                                 */
/* ------------------------------------------------------------------------ */

bool selftest__run_config_builtin_manage_case(void) {
    /* Called directly within selftest's own built-in process context. */
    const char *current_ssid = config__get_wifi_ap_ssid();
    const char *current_password = config__get_wifi_ap_password();
    const bruce_config_startup_apps_t *current_apps = config__get_startup_apps();
    const bruce_config_hotkeys_t *current_hotkeys = config__get_hotkeys();
    if (current_ssid == NULL || current_password == NULL || current_apps == NULL ||
        current_apps->count > CONFIG__STARTUP_APP_MAX_COUNT || current_hotkeys == NULL ||
        current_hotkeys->count > CONFIG__HOTKEY_MAX_COUNT) {
        printf("[selftest] config/builtin-manage: FAIL (singleton)\n");
        return false;
    }

    int original_volume = config__get_sound_volume();
    bool original_buffered_rendering = config__get_display_buffered_rendering();
    bool original_dma_framebuffer = config__get_display_dma_framebuffer();
    char original_ssid[CONFIG__WIFI_SSID_MAX_LEN + 1];
    char original_password[CONFIG__WIFI_PASSWORD_MAX_LEN + 1];
    char original_app_storage[CONFIG__STARTUP_APP_MAX_COUNT][CONFIG__STARTUP_APP_MAX_LEN + 1] = {0};
    const char *original_apps[CONFIG__STARTUP_APP_MAX_COUNT] = {0};
    size_t original_app_count = current_apps->count;
    char original_hotkey_storage[CONFIG__HOTKEY_MAX_COUNT][CONFIG__HOTKEY_MAX_LEN + 1] = {0};
    char original_action_storage[CONFIG__HOTKEY_MAX_COUNT][CONFIG__HOTKEY_ACTION_MAX_LEN + 1] = {0};
    bruce_config_hotkey_t original_hotkeys[CONFIG__HOTKEY_MAX_COUNT] = {0};
    size_t original_hotkey_count = current_hotkeys->count;
    snprintf(original_ssid, sizeof(original_ssid), "%s", current_ssid);
    snprintf(original_password, sizeof(original_password), "%s", current_password);
    for (size_t i = 0; i < original_app_count; ++i) {
        snprintf(original_app_storage[i], sizeof(original_app_storage[i]), "%s", current_apps->items[i]);
        original_apps[i] = original_app_storage[i];
    }
    for (size_t i = 0; i < original_hotkey_count; ++i) {
        snprintf(
            original_hotkey_storage[i],
            sizeof(original_hotkey_storage[i]),
            "%s",
            current_hotkeys->items[i].key
        );
        snprintf(
            original_action_storage[i],
            sizeof(original_action_storage[i]),
            "%s",
            current_hotkeys->items[i].action
        );
        original_hotkeys[i].key = original_hotkey_storage[i];
        original_hotkeys[i].action = original_action_storage[i];
    }

    bruce_result_t set_general = config__set_sound_volume(42);
    int general_value = config__get_sound_volume();
    bruce_result_t set_buffered_rendering = config__set_display_buffered_rendering(false);
    bool buffered_rendering_value = config__get_display_buffered_rendering();
    char *display_buffered_argv[] = {"config", "display", "buffered", "off"};
    bruce_result_t cli_display = config_app_main(4, display_buffered_argv);
    bruce_result_t set_dma_framebuffer = config__set_display_dma_framebuffer(false);
    bool dma_framebuffer_value = config__get_display_dma_framebuffer();

    bruce_result_t set_protected = config__set_wifi_ap("SelftestNet", "selftestpwd");
    const char *ssid = config__get_wifi_ap_ssid();
    const char *password = config__get_wifi_ap_password();
    bool string_values = ssid != NULL && password != NULL &&
                         strcmp(ssid, "SelftestNet") == 0 && strcmp(password, "selftestpwd") == 0;

    const char *new_apps[] = {"clock", "terminal", "webui"};
    bruce_result_t set_array = config__set_startup_apps(new_apps, 3);
    const bruce_config_startup_apps_t *apps = config__get_startup_apps();
    bool array_values = set_array == BRUCE_OK && apps != NULL && apps->count == 3 &&
                        strcmp(apps->items[0], "clock") == 0 && strcmp(apps->items[1], "terminal") == 0 &&
                        strcmp(apps->items[2], "webui") == 0;

    bruce_result_t add_duplicate = config__add_startup_app("clock");
    bruce_result_t add_app = config__add_startup_app("settings");
    bruce_result_t remove_app = config__remove_startup_app("terminal");
    bruce_result_t remove_missing = config__remove_startup_app("missing");
    apps = config__get_startup_apps();
    bool list_mutations = add_duplicate == BRUCE_OK && add_app == BRUCE_OK && remove_app == BRUCE_OK &&
                          remove_missing == BRUCE_ERR_NOT_FOUND && apps != NULL && apps->count == 3 &&
                          strcmp(apps->items[0], "clock") == 0 && strcmp(apps->items[1], "webui") == 0 &&
                          strcmp(apps->items[2], "settings") == 0;

    char *startup_add_argv[] = {"config", "system", "startup", "add", "selftest-startup"};
    char *startup_remove_argv[] = {"config", "system", "startup", "remove", "selftest-startup"};
    bruce_result_t cli_add = config_app_main(5, startup_add_argv);
    bruce_result_t cli_remove = config_app_main(5, startup_remove_argv);
    apps = config__get_startup_apps();
    bool cli_mutations = cli_add == BRUCE_OK && cli_remove == BRUCE_OK && apps != NULL && apps->count == 3;

    const bruce_config_hotkey_t test_hotkeys[] = {
        {"ctrl + x", "process switch next"}
    };
    bruce_result_t set_hotkeys = config__set_hotkeys(test_hotkeys, 1);
    const bruce_config_hotkeys_t *hotkeys = config__get_hotkeys();
    bool hotkey_values = set_hotkeys == BRUCE_OK && hotkeys != NULL && hotkeys->count == 1 &&
                         strcmp(hotkeys->items[0].key, "ctrl + x") == 0 &&
                         strcmp(hotkeys->items[0].action, "process switch next") == 0;

    char *json = NULL;
    size_t json_size = 0;
    bool read_json = storage__read_file(CONFIG__FILE_PATH, &json, &json_size);
    bool schema =
        read_json && json_size > 0 && strstr(json, "\"startup\"") != NULL &&
        strstr(json, "\"hotkeys\"") != NULL && strstr(json, "\"bufferedRendering\"") != NULL &&
        strstr(json, "\"dmaFramebuffer\"") != NULL && strstr(json, "\"theme\"") != NULL &&
        strstr(json, "\"display\"") != NULL && strstr(json, "\"time\"") != NULL &&
        strstr(json, "\"sound\"") != NULL && strstr(json, "\"led\"") != NULL && strstr(json, ":\t") == NULL &&
        strstr(json, "\"priColor\"") == NULL && strstr(json, "\"displayBufferedRendering\"") == NULL &&
        strstr(json, "\"startupApp\":") == NULL && strstr(json, "\"qrCodes\"") == NULL &&
        strstr(json, "\"evilWifiNames\"") == NULL && strstr(json, "\"evilWifiEndpoints\"") == NULL &&
        strstr(json, "\"evilWifiPasswordMode\"") == NULL;
    storage__free(json);

    bool restored = config__set_sound_volume(original_volume) == BRUCE_OK &&
                    config__set_display_buffered_rendering(original_buffered_rendering) == BRUCE_OK &&
                    config__set_display_dma_framebuffer(original_dma_framebuffer) == BRUCE_OK &&
                    config__set_wifi_ap(original_ssid, original_password) == BRUCE_OK &&
                    config__set_startup_apps(original_apps, original_app_count) == BRUCE_OK &&
                    config__set_hotkeys(original_hotkeys, original_hotkey_count) == BRUCE_OK;

    bool ok = set_general == BRUCE_OK && general_value == 42 && set_buffered_rendering == BRUCE_OK &&
              !buffered_rendering_value && cli_display == BRUCE_OK && set_dma_framebuffer == BRUCE_OK &&
              !dma_framebuffer_value && set_protected == BRUCE_OK && string_values && array_values &&
              list_mutations && cli_mutations && hotkey_values && schema && restored;
    printf(
        "[selftest] config/builtin-manage: %s (general=%d array=%d schema=%d)\n",
        ok ? "OK" : "FAIL",
        general_value,
        array_values,
        schema
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_config_theme_case                                          */
/* ------------------------------------------------------------------------ */

/* Covers Core's ten color_* roles (round-trip + config__parse_theme_color()'s
 * tolerant hex forms) and modules/config's `theme` subcommand, which owns
 * the named-preset catalog Core deliberately has no notion of (see
 * core_sdk/config.h). */
bool selftest__run_config_theme_case(void) {
    uint16_t original_primary = config__get_color_primary();
    uint16_t original_secondary = config__get_color_secondary();
    uint16_t original_background = config__get_color_background();
    uint16_t original_surface = config__get_color_surface();
    uint16_t original_text = config__get_color_text();
    uint16_t original_text_muted = config__get_color_text_muted();
    uint16_t original_border = config__get_color_border();
    uint16_t original_success = config__get_color_success();
    uint16_t original_warning = config__get_color_warning();
    uint16_t original_error = config__get_color_error();

    bool set_ok = config__set_color_primary(0x1234) == BRUCE_OK &&
                 config__set_color_secondary(0x2345) == BRUCE_OK &&
                 config__set_color_background(0x3456) == BRUCE_OK &&
                 config__set_color_surface(0x4567) == BRUCE_OK && config__set_color_text(0x5678) == BRUCE_OK &&
                 config__set_color_text_muted(0x6789) == BRUCE_OK &&
                 config__set_color_border(0x789A) == BRUCE_OK && config__set_color_success(0x89AB) == BRUCE_OK &&
                 config__set_color_warning(0x9ABC) == BRUCE_OK && config__set_color_error(0xABCD) == BRUCE_OK;
    bool roundtrip = set_ok && config__get_color_primary() == 0x1234 &&
                     config__get_color_secondary() == 0x2345 && config__get_color_background() == 0x3456 &&
                     config__get_color_surface() == 0x4567 && config__get_color_text() == 0x5678 &&
                     config__get_color_text_muted() == 0x6789 && config__get_color_border() == 0x789A &&
                     config__get_color_success() == 0x89AB && config__get_color_warning() == 0x9ABC &&
                     config__get_color_error() == 0xABCD;

    /* Native RGB565, "#RRGGBB", "RRGGBB", and "#RGB" shorthand all parse;
     * garbage and empty/NULL input are rejected. */
    uint16_t parsed = 0;
    bool parse_native = config__parse_theme_color("a80f", &parsed) && parsed == 0xA80F;
    bool parse_rgb888_hash = config__parse_theme_color("#FF0000", &parsed) && parsed == 0xF800;
    bool parse_rgb888 = config__parse_theme_color("00FF00", &parsed) && parsed == 0x07E0;
    bool parse_shorthand = config__parse_theme_color("#00F", &parsed) && parsed == 0x001F;
    bool parse_invalid = !config__parse_theme_color("zz", &parsed) && !config__parse_theme_color("", &parsed) &&
                         !config__parse_theme_color(NULL, &parsed);

    char *json = NULL;
    size_t json_size = 0;
    bool read_json = storage__read_file(CONFIG__FILE_PATH, &json, &json_size);
    bool schema = read_json && json_size > 0 && strstr(json, "\"surface\"") != NULL &&
                 strstr(json, "\"textMuted\"") != NULL && strstr(json, "\"border\"") != NULL &&
                 strstr(json, "\"success\"") != NULL && strstr(json, "\"warning\"") != NULL &&
                 strstr(json, "\"error\"") != NULL;
    storage__free(json);

    char *theme_set_argv[] = {"config", "theme", "set", "Dracula"};
    bruce_result_t cli_set = config_app_main(4, theme_set_argv);
    bool preset_applied = cli_set == BRUCE_OK && config__get_color_primary() == 0xBC9F &&
                          config__get_color_background() == 0x2946;

    /* "dark-gray" matches the "Dark Gray" preset name case- and
     * separator-insensitively, so a CLI caller doesn't need to shell-quote
     * a multi-word name. */
    char *theme_alias_argv[] = {"config", "theme", "set", "dark-gray"};
    bruce_result_t cli_alias = config_app_main(4, theme_alias_argv);
    bool alias_applied = cli_alias == BRUCE_OK && config__get_color_primary() == 0x8430;

    char *theme_set_color_argv[] = {"config", "theme", "set-color", "error", "#123456"};
    bruce_result_t cli_set_color = config_app_main(5, theme_set_color_argv);
    bool set_color_applied = cli_set_color == BRUCE_OK && config__get_color_error() == 0x11AA;

    char *theme_bad_argv[] = {"config", "theme", "set", "not-a-real-theme"};
    bruce_result_t cli_bad = config_app_main(4, theme_bad_argv);
    bool bad_rejected = cli_bad != BRUCE_OK;

    bool restored = config__set_color_primary(original_primary) == BRUCE_OK &&
                    config__set_color_secondary(original_secondary) == BRUCE_OK &&
                    config__set_color_background(original_background) == BRUCE_OK &&
                    config__set_color_surface(original_surface) == BRUCE_OK &&
                    config__set_color_text(original_text) == BRUCE_OK &&
                    config__set_color_text_muted(original_text_muted) == BRUCE_OK &&
                    config__set_color_border(original_border) == BRUCE_OK &&
                    config__set_color_success(original_success) == BRUCE_OK &&
                    config__set_color_warning(original_warning) == BRUCE_OK &&
                    config__set_color_error(original_error) == BRUCE_OK;

    bool parsing = parse_native && parse_rgb888_hash && parse_rgb888 && parse_shorthand && parse_invalid;
    bool ok = roundtrip && parsing && schema && preset_applied && alias_applied && set_color_applied &&
              bad_rejected && restored;
    printf(
        "[selftest] config/theme: %s (roundtrip=%d parse=%d preset=%d alias=%d setcolor=%d schema=%d)\n",
        ok ? "OK" : "FAIL",
        roundtrip,
        parsing,
        preset_applied,
        alias_applied,
        set_color_applied,
        schema
    );
    return ok;
}
