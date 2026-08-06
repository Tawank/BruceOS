#include "esp_log_level.h"
#include <stdio.h>

#include "core_sdk/loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"

#include "core/autostart/autostart.h"
#include "core/config/config.h"
#include "core/device/device.h"
#include "core/display/display.h"
#include "core/event_loop/event_loop.h"
#include "core/process/process.h"
#include "core/stdio/stdio.h"
#include "core/storage/storage.h"

#include "modules/apps/apps_app.h"
#include "modules/bluetooth/bluetooth_app.h"
#include "modules/bluetooth_hid/bluetooth_hid_app.h"
#include "modules/bnu/bnu_app.h"
#include "modules/bootanimation/bootanimation_app.h"
#include "modules/bparted/bparted_app.h"
#include "modules/bruce_launcher/bruce_launcher_app.h"
#include "modules/clock/clock_app.h"
#include "modules/config/config_app.h"
#include "modules/device_bus/device_bus_app.h"
#include "modules/filemanager/filemanager_app.h"
#include "modules/input/input_app.h"
#include "modules/ir/ir_app.h"
#include "modules/loaders/elf/elf_loader_app.h"
#include "modules/loaders/image/image_loader_app.h"
#include "modules/loaders/js/js_loader_app.h"
#include "modules/notification_service/notification_service.h"
#include "modules/nrf24/nrf24_app.h"
#include "modules/privileged/permissions/permissions_app.h"
#include "modules/selftest/selftest.h"
#include "modules/shell/shell_app.h"
#include "modules/ssh/ssh_app.h"
#include "modules/tcp/tcp_app.h"
#include "modules/text/text_app.h"
#include "modules/utils/help/help_app.h"
#include "modules/utils/launcher/launcher_app.h"
#include "modules/utils/notify/notify_app.h"
#include "modules/utils/process/process_app.h"
#include "modules/utils/serial_commands/serial_commands_app.h"
#include "modules/utils/terminal/terminal_app.h"
#include "modules/webui/webui_app.h"
#include "modules/wifi/wifi_app.h"

#define LAUNCHER_STACK_BYTES 3072u
#define INPUT_STACK_BYTES 3072u
#define DEVICE_BUS_STACK_BYTES 3072u
#define SERIAL_COMMANDS_STACK_BYTES 3072u
#define SELFTEST_STACK_BYTES 8192u
#define PERMISSIONS_STACK_BYTES 8192u
#define SHELL_STACK_BYTES 4096u
#define SSH_STACK_BYTES 16384u
#define SSH_KEYGEN_STACK_BYTES 12288u
/* bparted's GUI holds its whole screen - the running layout, the next-boot
 * layout, and every rendered row - in one ~3 KB frame that stays live while
 * dialog__choice() draws on top of it; comfortably over the default 4096
 * bytes. */
#define BPARTED_STACK_BYTES 8192u

void app_runner__register_defaults(void) {
    (void)app_runner__register("launcher", launcher_app_main, LAUNCHER_STACK_BYTES);
    (void)app_runner__register("bootanimation", bootanimation_app_main, 0);
    (void)app_runner__register("input", input_app_main, INPUT_STACK_BYTES);
    (void)app_runner__register("device_bus", device_bus_app_main, DEVICE_BUS_STACK_BYTES);
    (void)app_runner__register("bruce_launcher", bruce_launcher_app_main, 0);
    (void)app_runner__register("apps", apps_app_main, 0);
    (void)app_runner__register("filemanager", filemanager_app_main, 0);
    (void)app_runner__register("clock", clock_app_main, 0);
    (void)app_runner__register("config", config_app_main, 0);
    (void)app_runner__register("permissions", permissions_app_main, PERMISSIONS_STACK_BYTES);
    (void)app_runner__register("wifi", wifi_app_main, 0);
    (void)app_runner__register("webui", webui_app_main, 0);
    (void)app_runner__register("bluetooth", bluetooth_app_main, 0);
    (void)app_runner__register("bluetooth_hid_app", bluetooth_hid_app_main, 0);
    (void)app_runner__register("ir", ir_app_main, 0);
    (void)app_runner__register("nrf24", nrf24_app_main, 0);
    (void)app_runner__register("bparted", bparted_app_main, BPARTED_STACK_BYTES);
    (void)app_runner__register("selftest", selftest_app_main, SELFTEST_STACK_BYTES);
    (void)app_runner__register("terminal", terminal_app_main, 0);
    (void)app_runner__register("shell", shell_app_main, SHELL_STACK_BYTES);
    (void)app_runner__register("serial_commands", serial_commands_app_main, SERIAL_COMMANDS_STACK_BYTES);
    (void)app_runner__register("process", process_app_main, 0);
    (void)app_runner__register("help", help_app_main, 0);
    (void)app_runner__register("pwd", bnu_pwd_app_main, 0);
    (void)app_runner__register("cd", bnu_cd_app_main, 0);
    (void)app_runner__register("ls", bnu_ls_app_main, 0);
    (void)app_runner__register("lsblk", bnu_lsblk_app_main, 0);
    (void)app_runner__register("mount", bnu_mount_app_main, 0);
    (void)app_runner__register("unmount", bnu_unmount_app_main, 0);
    (void)app_runner__register("free", bnu_free_app_main, 0);
    (void)app_runner__register("top", bnu_top_app_main, 0);
    (void)app_runner__register("mkdir", bnu_mkdir_app_main, 0);
    (void)app_runner__register("touch", bnu_touch_app_main, 0);
    (void)app_runner__register("cat", bnu_cat_app_main, 0);
    (void)app_runner__register("elf", elf_loader__app_main, 0);
    (void)app_runner__register("js", js_loader__app_main, 0);
    (void)app_runner__register("image", image_app_main, 0);
    (void)app_runner__register("image_viewer", image_viewer_app_main, 0);
    (void)app_runner__register("text", text_app_main, 0);
    (void)app_runner__register("notify", notify_app_main, 0);
    (void)app_runner__register("notification_service", notification_service_main, 0);
    (void)app_runner__register("tcp", tcp_app_main, 0);
    (void)app_runner__register("ssh", ssh_app_main, SSH_STACK_BYTES);
    (void)app_runner__register("ssh-keygen", ssh_keygen_app_main, SSH_KEYGEN_STACK_BYTES);

    (void)app_runner__register_loader(".elf", 10, elf_loader__run_path);
    (void)app_runner__register_loader(".js", 20, js_loader__run_path);
    (void)app_runner__register_loader(".sh", 25, shell_loader__run_path);
    (void)app_runner__register_loader(".jpg", 30, image_loader__run_path);
    (void)app_runner__register_loader(".jpeg", 30, image_loader__run_path);
    (void)app_runner__register_loader(".png", 30, image_loader__run_path);
    (void)app_runner__register_loader(".gif", 30, image_loader__run_path);
    (void)app_runner__register_loader(".txt", 40, text__run_path);
    (void)app_runner__register_loader(".json", 40, text__run_path);
    (void)app_runner__register_loader(".conf", 40, text__run_path);

    elf_loader__init();
}

bool init_storage(void) {
    bool storage_ok = storage__init();
    if (!storage_ok) printf("Storage initialization failed\n");
    return storage_ok;
}

bool init_user_interface(void) {
    bool ui_ok = display__init() == BRUCE_OK;
    if (!ui_ok) printf("Display initialization failed; continuing without LCD\n");
    return ui_ok;
}

/* True when nothing in this boot can back memory__external_alloc(): no PSRAM
 * (boards like m5stack-cplus2 don't have any) and no "swap" partition either
 * (partitions.csv no longer carries one - see its header comment - so a
 * fresh device has none until core/partition_manager's user table stages
 * one). bruce_launcher builds its whole menu tree through
 * memory__external_alloc() (see bruce_launcher_menu.c's
 * bruce_launcher__parse_json()), so this is exactly the state that makes
 * "launcher -s" (a default startup app) fail every single time it's
 * (re)started, printing "Failed to load launcher configuration" in a tight
 * respawn loop and never actually booting. */
static bool app_main__external_memory_missing(void) {
    bruce_memory_stats_t stats;
    if (memory__get_stats(&stats) != BRUCE_OK) return false;
    return stats.psram_total == 0 && stats.swap_total == 0;
}

/* Blocks until `process_id` (a positive app_runner__run_command() result)
 * exits, discarding its status; a non-positive id is a no-op. Shared by
 * app_main__recover_missing_partitions() for both the services it starts
 * ahead of bparted and bparted itself. */
static void app_main__wait_for_exit(int process_id) {
    if (process_id <= 0) return;
    bruce_process_status_t status;
    for (;;) {
        bruce_result_t waited = process__wait_status((bruce_process_id_t)process_id, 200, &status);
        if (waited != BRUCE_ERR_TIMEOUT) return;
    }
}

/* Runs "bparted" before autostart__run() so the user can create a "swap"
 * partition (or fix up storage) while nothing that needs it - the launcher
 * included - has started yet. "GUI=1" only when there's a screen to drive it
 * (see autostart__run()'s own ui_ok-gated "GUI=1" convention); headless
 * boards are left to fix this over a serial "bparted create ..." session,
 * same as any other storage problem today. */
static void app_main__recover_missing_partitions(const char *reason, bool ui_ok) {
    printf("%s; run \"bparted\" to create a partition\n", reason);
#if CONFIG_BRUCE_QEMU_TEST_MODE
    /* Nothing here can answer an interactive dialog on an unattended/CI
     * boot - dialog__choice() would just block forever waiting for input
     * nobody is going to send, wedging app_main() before it ever reaches
     * this file's "#if CONFIG_BRUCE_QEMU_TEST_MODE" selftest launch below. */
    (void)ui_ok;
    return;
#else
    if (!ui_ok) return;

    /* bparted's GUI reads button/touch events through the "input" service
     * (which itself needs "device_bus" for I2C), same as every other GUI
     * app - but this runs ahead of autostart__run(), before config's
     * startup list would normally bring them up. Start both here, run
     * bparted, then stop them again so autostart__run() right after this
     * starts them (and everything else the user has configured) exactly as
     * it would on a boot that never needed this recovery path, with no two
     * instances of either ever fighting over the same hardware. */
    int device_bus_id = app_runner__run_command("device_bus", BRUCE_LAUNCH_BACKGROUND);
    int input_id = app_runner__run_command("input", BRUCE_LAUNCH_BACKGROUND);

    int bparted_id = app_runner__run_command("GUI=1 bparted", BRUCE_LAUNCH_FOREGROUND);
    app_main__wait_for_exit(bparted_id);

    if (input_id > 0) {
        (void)process__terminate((bruce_process_id_t)input_id);
        app_main__wait_for_exit(input_id);
    }
    if (device_bus_id > 0) {
        (void)process__terminate((bruce_process_id_t)device_bus_id);
        app_main__wait_for_exit(device_bus_id);
    }
#endif
}

void app_main(void) {
    device__power_hold_init();

    bool storage_ok = init_storage();
    if (storage_ok && !config__init()) printf("Configuration is unavailable; using in-memory defaults\n");
    if (storage_ok && !process__environment_init()) {
        printf("Global environment configuration is unavailable\n");
    }
    if (stdio__init() != BRUCE_OK) printf("USB serial console initialization failed\n");
    if (event_loop__init() != BRUCE_OK) printf("Core event loop initialization failed\n");

    bool ui_ok = init_user_interface();

    app_runner__register_defaults();
    /* Neither condition can change without a reboot (see
     * app_main__recover_missing_partitions()'s doc comment), so whichever is
     * true now is still true after that call returns - whether the user
     * fixed it (and rebooted from inside bparted, in which case app_main()
     * never gets this far again this boot) or backed out without fixing it.
     * In the latter case "launcher" (config's default "launcher -s") is
     * skipped below: it can only fail the exact same way again, and its own
     * "-s" supervisor has no backoff (see modules/utils/launcher/
     * launcher_app.c), so starting it anyway is just an infinite "Failed to
     * load launcher configuration" print loop, not a real attempt. */
    const char *skip_command = NULL;
    if (!storage_ok) {
        app_main__recover_missing_partitions("No usable partitions found", ui_ok);
        skip_command = "launcher";
    } else if (app_main__external_memory_missing()) {
        app_main__recover_missing_partitions("No PSRAM or swap partition found", ui_ok);
        skip_command = "launcher";
    }
    autostart__run(ui_ok, skip_command);

#if CONFIG_BRUCE_QEMU_TEST_MODE
#define MAIN_SERIAL_READY_TIMEOUT_MS 1000
    if (!serial_commands__wait_ready(MAIN_SERIAL_READY_TIMEOUT_MS)) {
        printf("Serial command frontend failed to start\n");
    }
    printf("\n\nSELFTEST READY\n\n");
    fflush(stdout);
    app_runner__run_command("selftest", BRUCE_LAUNCH_BACKGROUND);
    return;
#endif

    esp_log_level_set("*", ESP_LOG_WARN);
}
