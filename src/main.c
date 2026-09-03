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
#include "modules/archive/archive_app.h"
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

#define BPARTED_STACK_BYTES 8192u
#define BROWSER_STACK_BYTES 16384u
#define CONFIG_STACK_BYTES 8192u
#define DEVICE_BUS_STACK_BYTES 3072u
#define HTTP_STACK_BYTES 16384u
#define INPUT_STACK_BYTES 3072u
#define MENU_STACK_BYTES 4096u
#define NOTIFICATION_SERVICE_STACK_BYTES 2048u
#define PERMISSIONS_STACK_BYTES 8192u
#define SERIAL_COMMANDS_STACK_BYTES 3072u
/* 4096 used to be enough, but proved too tight once brace expansion
 * (shell_brace.c) gave shell_parser.c's word tokenizer one more call layer
 * to go through on its way to a "$(...)" command substitution's own already-
 * deep call chain (shell_parser__words() -> ... -> shell_executor__run_substitution()
 * -> shell_executor__capture_external(), still all on this task's own stack
 * up to the point where the substitution's child process is actually
 * spawned) -- a real stack overflow (see shell_app.c's
 * shell_app__collect_heredoc_body() doc comment for the same class of bug
 * before), not merely a margin-of-safety bump. Matches every other app here
 * with genuinely interpreter-like logic (CONFIG_STACK_BYTES,
 * PERMISSIONS_STACK_BYTES, STORAGE_COMMANDS_STACK_BYTES, TERMINAL_STACK_BYTES
 * all below) rather than standing out as unusually tight among them. */
#define SHELL_STACK_BYTES 8192u
#define SSH_STACK_BYTES 16384u
#define STORAGE_COMMANDS_STACK_BYTES 8192u
#define TERMINAL_STACK_BYTES 8192u
#define WIFI_STACK_BYTES 8192u

void app_runner__register_defaults(void) {
    /* System - boot, launcher, config, diagnostics, power. */
    app_runner__register("apps", "Browse installed apps", "System", apps_app_main, 0);
    app_runner__register("bootanimation", "Play the boot animation", "System", bootanimation_app_main, 0);
    app_runner__register("bruce_launcher", "Open the Bruce launcher", "System", bruce_launcher_app_main, 0);
    app_runner__register("clock", "Show clock and timer tools", "System", clock_app_main, 0);
    app_runner__register("config", "Configure Bruce settings", "System", config_app_main, CONFIG_STACK_BYTES);
    app_runner__register("date", "Show or set the date and time", "System", bnu_date_app_main, 0);
    app_runner__register("free", "Show memory usage", "System", bnu_free_app_main, 0);
    app_runner__register("memorydump", "Dump a validated memory range", "System", bnu_memorydump_app_main, 0);
    app_runner__register("launcher", "Launches configured launcher", "System", launcher_app_main, 3072u);
    app_runner__register("man", "List commands or show command manuals", "System", man_app_main, 0);
    app_runner__register("menu", "Open the system menu", "System", system_menu_app_main, MENU_STACK_BYTES);
    app_runner__register(
        "notification_service", "The notification service", "System", notification_service_main, 0
    );
    app_runner__register("notify", "Send a notification", "System", notify_app_main, 0);
    app_runner__register(
        "permissions",
        "Manage application permissions",
        "System",
        permissions_app_main,
        PERMISSIONS_STACK_BYTES
    );
    app_runner__register("process", "List and manage processes", "System", process_app_main, 0);
    app_runner__register("reboot", "Restart the device", "System", bnu_reboot_app_main, 0);
    app_runner__register("selftest", "Hardware and Core self-tests", "System", selftest_app_main, 8192u);
    app_runner__register("shutdown", "Power off the device", "System", bnu_shutdown_app_main, 0);
    app_runner__register("sleep", "Pause for a duration", "System", bnu_sleep_app_main, 0);
    app_runner__register("top", "Processes, CPU, RAM usage", "System", bnu_top_app_main, 0);

    /* Storage - filesystems, partitions, and file management. */
    app_runner__register(
        "archive", "Browse a .zip or .tar.gz/.tgz archive's contents", "Storage", archive_app_main, 8192
    );
    app_runner__register(
        "archive-extract", "Extract a .zip or .tar.gz/.tgz archive", "Storage", bnu_archive_extract_app_main, 0
    );
    app_runner__register("bparted", "Manage partitions", "Storage", bparted_app_main, BPARTED_STACK_BYTES);
    app_runner__register("cat", "Print file contents", "Storage", bnu_cat_app_main, 0);
    app_runner__register("cp", "Copy a file", "Storage", bnu_cp_app_main, 0);
    app_runner__register("df", "Show mounted filesystems' space usage", "Storage", bnu_df_app_main, 0);
    app_runner__register("du", "Show a file or directory tree's disk usage", "Storage", bnu_du_app_main, 0);
    app_runner__register("file", "Identify a file's type", "Storage", bnu_file_app_main, 0);
    app_runner__register("filemanager", "Browse and manage files", "Storage", filemanager_app_main, 8192);
    app_runner__register("head", "Print the first part of a file", "Storage", bnu_head_app_main, 0);
    app_runner__register("ls", "List directory contents", "Storage", bnu_ls_app_main, 0);
    app_runner__register("lsblk", "List storage devices", "Storage", bnu_lsblk_app_main, 0);
    app_runner__register("mkdir", "Create directories", "Storage", bnu_mkdir_app_main, 0);
    app_runner__register("mount", "Mount storage devices", "Storage", bnu_mount_app_main, 0);
    app_runner__register("mv", "Move or rename a file", "Storage", bnu_mv_app_main, 0);
    app_runner__register("pwd", "Print the current directory", "Storage", bnu_pwd_app_main, 0);
    app_runner__register("rm", "Remove files and directories", "Storage", bnu_rm_app_main, 0);
    app_runner__register(
        "storage",
        "Manage files (list/remove/mkdir/rename/read/write)",
        "Storage",
        storage_commands_app_main,
        STORAGE_COMMANDS_STACK_BYTES
    );
    app_runner__register("tail", "Print the last part of a file", "Storage", bnu_tail_app_main, 0);
    app_runner__register("tar", "Create, list, or extract a .tar.gz archive", "Storage", bnu_tar_app_main, 0);
    app_runner__register("touch", "Create files or update timestamps", "Storage", bnu_touch_app_main, 0);
    app_runner__register("unmount", "Unmount storage devices", "Storage", bnu_unmount_app_main, 0);
    app_runner__register("unzip", "List or extract a .zip archive", "Storage", bnu_unzip_app_main, 0);
    app_runner__register("zip", "Create a .zip archive", "Storage", bnu_zip_app_main, 0);

    /* Network - Wi-Fi, web, and remote-access tools. */
    app_runner__register("browser", "Browse web pages", "Network", browser_app_main, BROWSER_STACK_BYTES);
    app_runner__register("curl", "Transfer data from URLs", "Network", bnu_curl_app_main, HTTP_STACK_BYTES);
    app_runner__register("ssh", "Connect to an SSH server", "Network", ssh_app_main, SSH_STACK_BYTES);
    app_runner__register("ssh-keygen", "Generate SSH keys", "Network", ssh_keygen_app_main, SSH_STACK_BYTES);
    app_runner__register("tcp", "Connect to a TCP server", "Network", tcp_app_main, 0);
    app_runner__register("webui", "Start the web interface", "Network", webui_app_main, 0);
    app_runner__register("wget", "Download files from URLs", "Network", bnu_wget_app_main, HTTP_STACK_BYTES);
    app_runner__register("wifi", "Manage Wi-Fi connections", "Network", wifi_app_main, WIFI_STACK_BYTES);

    /* Radio - Bluetooth, infrared, NRF24, and other device buses. */
    app_runner__register("bluetooth", "Scan and manage Bluetooth devices", "Radio", bluetooth_app_main, 0);
    app_runner__register(
        "bluetooth_hid_app", "Use Bruce as a Bluetooth HID peripheral", "Radio", bluetooth_hid_app_main, 0
    );
    app_runner__register(
        "device_bus", "Inspect device buses", "Radio", device_bus_app_main, DEVICE_BUS_STACK_BYTES
    );
    app_runner__register("input", "Inspect input devices", "Radio", input_app_main, INPUT_STACK_BYTES);
    app_runner__register("ir", "Control infrared devices", "Radio", ir_app_main, 0);
    app_runner__register("nrf24", "Control NRF24 radios", "Radio", nrf24_app_main, 0);

    /* Runtime - ELF, JS, and Wasm application loaders. */
    app_runner__register("elf", "Run ELF applications", "Runtime", elf_loader__app_main, 0);
    app_runner__register("js", "Run JavaScript applications", "Runtime", js_loader__app_main, 0);
    app_runner__register("wasm", "Run WebAssembly applications", "Runtime", wasm_loader__app_main, 0);

    /* Shell - terminals, shells, and command frontends. */
    app_runner__register(
        "serial_commands",
        "Run the terminal command service",
        "Shell",
        serial_commands_app_main,
        SERIAL_COMMANDS_STACK_BYTES
    );
    app_runner__register("shell", "Run the command shell", "Shell", shell_app_main, SHELL_STACK_BYTES);
    app_runner__register("stty", "Configure terminal settings", "Shell", bnu_stty_app_main, 0);
    app_runner__register("terminal", "Open a terminal", "Shell", terminal_app_main, TERMINAL_STACK_BYTES);

    /* Content - viewing and editing text and images. */
    app_runner__register("base64", "Base64 encode or decode data", "Content", bnu_base64_app_main, 0);
    app_runner__register("crc32", "Print CRC-32 checksums", "Content", bnu_crc32_app_main, 0);
    app_runner__register("cut", "Extract fields or characters from lines", "Content", bnu_cut_app_main, 0);
    app_runner__register("grep", "Search for text in files or stdin", "Content", bnu_grep_app_main, 0);
    app_runner__register("gunzip", "Decompress a .gz file", "Content", bnu_gunzip_app_main, 0);
    app_runner__register("gzip", "Compress a file to .gz", "Content", bnu_gzip_app_main, 0);
    app_runner__register("image", "View image files", "Content", image_app_main, 8192);
    app_runner__register("less", "Page through text", "Content", bnu_less_app_main, 0);
    app_runner__register("md5sum", "Print MD5 checksums", "Content", bnu_md5sum_app_main, 0);
    app_runner__register("rev", "Reverse the characters of each line", "Content", bnu_rev_app_main, 0);
    app_runner__register("seq", "Print a sequence of numbers", "Content", bnu_seq_app_main, 0);
    app_runner__register("sha256sum", "Print SHA-256 checksums", "Content", bnu_sha256sum_app_main, 0);
    app_runner__register("sort", "Sort the lines of a file or stdin", "Content", bnu_sort_app_main, 0);
    app_runner__register("tee", "Copy stdin to stdout and to files", "Content", bnu_tee_app_main, 0);
    app_runner__register("text", "Edit text files", "Content", text_app_main, 0);
    app_runner__register("tr", "Translate, delete, or squeeze characters", "Content", bnu_tr_app_main, 0);
    app_runner__register("uniq", "Filter out repeated adjacent lines", "Content", bnu_uniq_app_main, 0);
    app_runner__register("wc", "Count lines, words, and bytes", "Content", bnu_wc_app_main, 0);
    app_runner__register("wl-copy", "Copy stdin or a file to the clipboard", "Content", bnu_wl_copy_app_main, 0);
    app_runner__register("wl-paste", "Print the clipboard to stdout", "Content", bnu_wl_paste_app_main, 0);
    app_runner__register("xxd", "Make a hexadecimal dump", "Content", bnu_xxd_app_main, 0);

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
