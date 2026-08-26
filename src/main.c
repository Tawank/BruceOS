#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_system.h"
#include <stdio.h>

#include "core_sdk/ext_mem_loader.h"

#include "core/autostart/autostart.h"
#include "core/config/config.h"
#include "core/device/device.h"
#include "core/disk/disk.h"
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
#include "modules/browser/browser_app.h"
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
#include "modules/loaders/wasm/wasm_loader_app.h"
#include "modules/notification_service/notification_service.h"
#include "modules/nrf24/nrf24_app.h"
#include "modules/privileged/permissions/permissions_app.h"
#include "modules/selftest/selftest.h"
#include "modules/shell/shell_app.h"
#include "modules/ssh/ssh_app.h"
#include "modules/system_menu/system_menu_app.h"
#include "modules/tcp/tcp_app.h"
#include "modules/text/text_app.h"
#include "modules/utils/launcher/launcher_app.h"
#include "modules/utils/man/man_app.h"
#include "modules/utils/notify/notify_app.h"
#include "modules/utils/process/process_app.h"
#include "modules/utils/serial_commands/serial_commands_app.h"
#include "modules/utils/storage_commands/storage_commands_app.h"
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
#define WIFI_STACK_BYTES 8192u
#define BPARTED_STACK_BYTES 8192u
#define STORAGE_COMMANDS_STACK_BYTES 8192u
#define NOTIFICATION_SERVICE_STACK_BYTES 2048u
#define MENU_STACK_BYTES 4096u
#define CONFIG_STACK_BYTES 8192u
#define BNU_HTTP_STACK_BYTES 16384u
#define BROWSER_STACK_BYTES 16384u

void app_runner__register_defaults(void) {
    app_runner__register("apps", "Browse and manage installed apps", apps_app_main, 0);
    app_runner__register("bluetooth", "Scan and manage Bluetooth devices", bluetooth_app_main, 0);
    app_runner__register(
        "bluetooth_hid_app", "Use Bruce as a Bluetooth HID peripheral", bluetooth_hid_app_main, 0
    );
    app_runner__register("bootanimation", "Play the boot animation", bootanimation_app_main, 0);
    app_runner__register("bparted", "Manage storage partitions", bparted_app_main, BPARTED_STACK_BYTES);
    app_runner__register("browser", "Browse web pages", browser_app_main, BROWSER_STACK_BYTES);
    app_runner__register("bruce_launcher", "Open the Bruce application launcher", bruce_launcher_app_main, 0);
    app_runner__register("cat", "Print file contents", bnu_cat_app_main, 0);
    app_runner__register("clock", "Show clock and timer tools", clock_app_main, 0);
    app_runner__register("config", "Configure Bruce settings", config_app_main, CONFIG_STACK_BYTES);
    app_runner__register("curl", "Transfer data from URLs", bnu_curl_app_main, BNU_HTTP_STACK_BYTES);
    app_runner__register(
        "device_bus", "Inspect and control device buses", device_bus_app_main, DEVICE_BUS_STACK_BYTES
    );
    app_runner__register("elf", "Run ELF applications", elf_loader__app_main, 0);
    app_runner__register("filemanager", "Browse and manage files", filemanager_app_main, 0);
    app_runner__register("free", "Show memory usage", bnu_free_app_main, 0);
    app_runner__register("image", "Decode image files", image_app_main, 0);
    app_runner__register("image_viewer", "View image files", image_viewer_app_main, 8192);
    app_runner__register("input", "Inspect input devices", input_app_main, INPUT_STACK_BYTES);
    app_runner__register("ir", "Control infrared devices", ir_app_main, 0);
    app_runner__register("js", "Run JavaScript applications", js_loader__app_main, 0);
    app_runner__register(
        "launcher", "Open the application launcher", launcher_app_main, LAUNCHER_STACK_BYTES
    );
    app_runner__register("less", "Page through text", bnu_less_app_main, 0);
    app_runner__register("ls", "List directory contents", bnu_ls_app_main, 0);
    app_runner__register("lsblk", "List storage devices", bnu_lsblk_app_main, 0);
    app_runner__register("man", "List commands or show command manuals", man_app_main, 0);
    app_runner__register("menu", "Open the system menu", system_menu_app_main, MENU_STACK_BYTES);
    app_runner__register("mkdir", "Create directories", bnu_mkdir_app_main, 0);
    app_runner__register("mount", "Mount storage devices", bnu_mount_app_main, 0);
    app_runner__register(
        "notification_service", "Run the notification service", notification_service_main, 0
    );
    app_runner__register("notify", "Send a notification", notify_app_main, 0);
    app_runner__register("nrf24", "Control NRF24 radios", nrf24_app_main, 0);
    app_runner__register(
        "permissions", "Manage application permissions", permissions_app_main, PERMISSIONS_STACK_BYTES
    );
    app_runner__register("process", "List and manage processes", process_app_main, 0);
    app_runner__register("pwd", "Print the current directory", bnu_pwd_app_main, 0);
    app_runner__register("reboot", "Restart the device", bnu_reboot_app_main, 0);
    app_runner__register("rm", "Remove files and directories", bnu_rm_app_main, 0);
    app_runner__register(
        "selftest", "Run hardware and Core self-tests", selftest_app_main, SELFTEST_STACK_BYTES
    );
    app_runner__register(
        "serial_commands",
        "Run the terminal command service",
        serial_commands_app_main,
        SERIAL_COMMANDS_STACK_BYTES
    );
    app_runner__register("shell", "Run the command shell", shell_app_main, SHELL_STACK_BYTES);
    app_runner__register("shutdown", "Power off the device", bnu_shutdown_app_main, 0);
    app_runner__register("ssh", "Connect to an SSH server", ssh_app_main, SSH_STACK_BYTES);
    app_runner__register("ssh-keygen", "Generate SSH keys", ssh_keygen_app_main, SSH_KEYGEN_STACK_BYTES);
    app_runner__register(
        "storage", "Manage files (list/remove/mkdir/rename/read/write)", storage_commands_app_main,
        STORAGE_COMMANDS_STACK_BYTES
    );
    app_runner__register("stty", "Configure terminal settings", bnu_stty_app_main, 0);
    app_runner__register("tcp", "Connect to a TCP server", tcp_app_main, 0);
    app_runner__register("terminal", "Open a terminal", terminal_app_main, 0);
    app_runner__register("text", "Edit text files", text_app_main, 0);
    app_runner__register("top", "Show processes and resource usage", bnu_top_app_main, 0);
    app_runner__register("touch", "Create files or update timestamps", bnu_touch_app_main, 0);
    app_runner__register("unmount", "Unmount storage devices", bnu_unmount_app_main, 0);
    app_runner__register("wasm", "Run WebAssembly applications", wasm_loader__app_main, 0);
    app_runner__register("webui", "Start the web interface", webui_app_main, 0);
    app_runner__register("wget", "Download files from URLs", bnu_wget_app_main, BNU_HTTP_STACK_BYTES);
    app_runner__register("wifi", "Manage Wi-Fi connections", wifi_app_main, WIFI_STACK_BYTES);

    app_runner__register_loader(".elf", "elf");
    app_runner__register_loader(".wasm", "wasm");
    app_runner__register_loader(".js", "js");
    app_runner__register_loader(".sh", "shell");
    app_runner__register_loader(".jpg", "image");
    app_runner__register_loader(".jpeg", "image");
    app_runner__register_loader(".png", "image");
    app_runner__register_loader(".gif", "image");
    app_runner__register_loader(".txt", "text");
    app_runner__register_loader(".json", "text");
    app_runner__register_loader(".conf", "text");

    elf_loader__init();
}

bool init_storage(void) {
    bool storage_ok = storage__init();
    if (!storage_ok) printf("Storage initialization failed\n");
    /* Independent of internal storage: a board with no SD slot at all
     * (BRUCE_SD_ENABLED unset) or no card inserted just leaves /sdcard
     * unmounted, so the result isn't checked here - `lsblk`/`mount` reflect
     * whatever actually happened, and storage__sd_mount_spi() already logs
     * a warning on failure. */
    disk__mount_sd_boot();
    return storage_ok;
}

bool init_user_interface(void) {
    bool ui_ok = display__init() == BRUCE_OK;
    if (!ui_ok) printf("Display initialization failed; continuing without LCD\n");
    return ui_ok;
}

#if CONFIG_BRUCE_QEMU_TEST_MODE
void quemu_test_mode__wait_for_serial_ready(void) {
#define MAIN_SERIAL_READY_TIMEOUT_MS 1000
    if (!serial_commands__wait_ready(MAIN_SERIAL_READY_TIMEOUT_MS)) {
        printf("Serial command frontend failed to start\n");
    }
    printf("\n\nSELFTEST READY\n\n");
    fflush(stdout);
    app_runner__run_command("selftest", BRUCE_LAUNCH_BACKGROUND);
    return;
}
#endif

void app_main(void) {
    ESP_LOGI("boot", "reset reason: %d\n", (int)esp_reset_reason());
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

    autostart__run(ui_ok);

#if CONFIG_BRUCE_QEMU_TEST_MODE
    quemu_test_mode__wait_for_serial_ready();
    return;
#endif

    esp_log_level_set("*", ESP_LOG_WARN);
}
