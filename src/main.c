#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_system.h"
#include <stdio.h>

#include "core_sdk/app_runner.h"
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
#include "modules/nc/nc_app.h"
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

/* One row per built-in command. `static const` so the compiler places the
 * whole table in flash (.rodata) - app_runner__register_all() (app_runner.c)
 * references it in place rather than copying it into RAM, which is also why
 * this can be exactly as long as the command list needs, with no registry
 * capacity to size or run into. */
static const bruce_app_descriptor_t s_default_apps[] = {
    /* System - boot, launcher, config, diagnostics, power. */
    {"apps", "Browse installed apps", "System", apps_app_main, 0},
    {"bootanimation", "Play the boot animation", "System", bootanimation_app_main, 0},
    {"bruce_launcher", "Open the Bruce launcher", "System", bruce_launcher_app_main, 0},
    {"clock", "Show clock and timer tools", "System", clock_app_main, 0},
    {"config", "Configure Bruce settings", "System", config_app_main, CONFIG_STACK_BYTES},
    {"date", "Show or set the date and time", "System", bnu_date_app_main, 0},
    {"free", "Show memory usage", "System", bnu_free_app_main, 0},
    {"memorydump", "Dump a validated memory range", "System", bnu_memorydump_app_main, 0},
    {"launcher", "Launches configured launcher", "System", launcher_app_main, 3072u},
    {"man", "List commands or show command manuals", "System", man_app_main, 0},
    {"menu", "Open the system menu", "System", system_menu_app_main, MENU_STACK_BYTES},
    {"notification_service", "The notification service", "System", notification_service_main, 0},
    {"notify", "Send a notification", "System", notify_app_main, 0},
    {"permissions", "Manage application permissions", "System", permissions_app_main, PERMISSIONS_STACK_BYTES},
    {"process", "List and manage processes", "System", process_app_main, 0},
    {"reboot", "Restart the device", "System", bnu_reboot_app_main, 0},
    {"selftest", "Hardware and Core self-tests", "System", selftest_app_main, 8192u},
    {"shutdown", "Power off the device", "System", bnu_shutdown_app_main, 0},
    {"sleep", "Pause for a duration", "System", bnu_sleep_app_main, 0},
    {"top", "Processes, CPU, RAM usage", "System", bnu_top_app_main, 0},

    /* Storage - filesystems, partitions, and file management. */
    {"archive", "Browse a .zip or .tar.gz/.tgz archive's contents", "Storage", archive_app_main, 8192},
    {"archive-extract", "Extract a .zip or .tar.gz/.tgz archive", "Storage", bnu_archive_extract_app_main, 0},
    {"bparted", "Manage partitions", "Storage", bparted_app_main, BPARTED_STACK_BYTES},
    {"cat", "Print file contents", "Storage", bnu_cat_app_main, 0},
    {"cp", "Copy a file", "Storage", bnu_cp_app_main, 0},
    {"df", "Show mounted filesystems' space usage", "Storage", bnu_df_app_main, 0},
    {"du", "Show a file or directory tree's disk usage", "Storage", bnu_du_app_main, 0},
    {"file", "Identify a file's type", "Storage", bnu_file_app_main, 0},
    {"filemanager", "Browse and manage files", "Storage", filemanager_app_main, 8192},
    {"head", "Print the first part of a file", "Storage", bnu_head_app_main, 0},
    {"ls", "List directory contents", "Storage", bnu_ls_app_main, 0},
    {"lsblk", "List storage devices", "Storage", bnu_lsblk_app_main, 0},
    {"mkdir", "Create directories", "Storage", bnu_mkdir_app_main, 0},
    {"mount", "Mount storage devices", "Storage", bnu_mount_app_main, 0},
    {"mv", "Move or rename a file", "Storage", bnu_mv_app_main, 0},
    {"pwd", "Print the current directory", "Storage", bnu_pwd_app_main, 0},
    {"rm", "Remove files and directories", "Storage", bnu_rm_app_main, 0},
    {"storage", "Manage files (list/remove/mkdir/rename/read/write)", "Storage", storage_commands_app_main,
     STORAGE_COMMANDS_STACK_BYTES},
    {"tail", "Print the last part of a file", "Storage", bnu_tail_app_main, 0},
    {"tar", "Create, list, or extract a .tar.gz archive", "Storage", bnu_tar_app_main, 0},
    {"touch", "Create files or update timestamps", "Storage", bnu_touch_app_main, 0},
    {"unmount", "Unmount storage devices", "Storage", bnu_unmount_app_main, 0},
    {"unzip", "List or extract a .zip archive", "Storage", bnu_unzip_app_main, 0},
    {"zip", "Create a .zip archive", "Storage", bnu_zip_app_main, 0},

    /* Network - Wi-Fi, web, and remote-access tools. */
    {"browser", "Browse web pages", "Network", browser_app_main, BROWSER_STACK_BYTES},
    {"curl", "Transfer data from URLs", "Network", bnu_curl_app_main, HTTP_STACK_BYTES},
    {"ssh", "Connect to an SSH server", "Network", ssh_app_main, SSH_STACK_BYTES},
    {"ssh-keygen", "Generate SSH keys", "Network", ssh_keygen_app_main, SSH_STACK_BYTES},
    {"nc", "Connect to or listen on a TCP socket", "Network", nc_app_main, 0},
    {"webui", "Start the web interface", "Network", webui_app_main, 0},
    {"wget", "Download files from URLs", "Network", bnu_wget_app_main, HTTP_STACK_BYTES},
    {"wifi", "Manage Wi-Fi connections", "Network", wifi_app_main, WIFI_STACK_BYTES},

    /* Radio - Bluetooth, infrared, NRF24, and other device buses. */
    {"bluetooth", "Scan and manage Bluetooth devices", "Radio", bluetooth_app_main, 0},
    {"bluetooth_hid_app", "Use Bruce as a Bluetooth HID peripheral", "Radio", bluetooth_hid_app_main, 0},
    {"device_bus", "Inspect device buses", "Radio", device_bus_app_main, DEVICE_BUS_STACK_BYTES},
    {"input", "Inspect input devices", "Radio", input_app_main, INPUT_STACK_BYTES},
    {"ir", "Control infrared devices", "Radio", ir_app_main, 0},
    {"nrf24", "Control NRF24 radios", "Radio", nrf24_app_main, 0},

    /* Runtime - ELF, JS, and Wasm application loaders. */
    {"elf", "Run ELF applications", "Runtime", elf_loader__app_main, 0},
    {"js", "Run JavaScript applications", "Runtime", js_loader__app_main, 0},
    {"wasm", "Run WebAssembly applications", "Runtime", wasm_loader__app_main, 0},

    /* Shell - terminals, shells, and command frontends. */
    {"serial_commands", "Run the terminal command service", "Shell", serial_commands_app_main,
     SERIAL_COMMANDS_STACK_BYTES},
    {"shell", "Run the command shell", "Shell", shell_app_main, SHELL_STACK_BYTES},
    {"stty", "Configure terminal settings", "Shell", bnu_stty_app_main, 0},
    {"terminal", "Open a terminal", "Shell", terminal_app_main, TERMINAL_STACK_BYTES},

    /* Content - viewing and editing text and images. */
    {"base64", "Base64 encode or decode data", "Content", bnu_base64_app_main, 0},
    {"crc32", "Print CRC-32 checksums", "Content", bnu_crc32_app_main, 0},
    {"cut", "Extract fields or characters from lines", "Content", bnu_cut_app_main, 0},
    {"grep", "Search for text in files or stdin", "Content", bnu_grep_app_main, 0},
    {"gunzip", "Decompress a .gz file", "Content", bnu_gunzip_app_main, 0},
    {"gzip", "Compress a file to .gz", "Content", bnu_gzip_app_main, 0},
    {"image", "View image files", "Content", image_app_main, 8192},
    {"less", "Page through text", "Content", bnu_less_app_main, 0},
    {"md5sum", "Print MD5 checksums", "Content", bnu_md5sum_app_main, 0},
    {"rev", "Reverse the characters of each line", "Content", bnu_rev_app_main, 0},
    {"seq", "Print a sequence of numbers", "Content", bnu_seq_app_main, 0},
    {"sha256sum", "Print SHA-256 checksums", "Content", bnu_sha256sum_app_main, 0},
    {"sort", "Sort the lines of a file or stdin", "Content", bnu_sort_app_main, 0},
    {"tee", "Copy stdin to stdout and to files", "Content", bnu_tee_app_main, 0},
    {"text", "Edit text files", "Content", text_app_main, 0},
    {"tr", "Translate, delete, or squeeze characters", "Content", bnu_tr_app_main, 0},
    {"uniq", "Filter out repeated adjacent lines", "Content", bnu_uniq_app_main, 0},
    {"wc", "Count lines, words, and bytes", "Content", bnu_wc_app_main, 0},
    {"wl-copy", "Copy stdin or a file to the clipboard", "Content", bnu_wl_copy_app_main, 0},
    {"wl-paste", "Print the clipboard to stdout", "Content", bnu_wl_paste_app_main, 0},
    {"xxd", "Make a hexadecimal dump", "Content", bnu_xxd_app_main, 0},
};

void app_runner__register_defaults(void) {
    bruce_result_t registered =
        app_runner__register_all(s_default_apps, sizeof(s_default_apps) / sizeof(s_default_apps[0]));
    if (registered != BRUCE_OK) {
        ESP_LOGE("boot", "app_runner__register_all() failed: %d (duplicate or malformed entry?)", (int)registered);
    }

    static const bruce_loader_descriptor_t default_loaders[] = {
        {".elf", "elf"},
        {".wasm", "wasm"},
        {".js", "js"},
        {".sh", "shell"},
        {".jpg", "image"},
        {".jpeg", "image"},
        {".png", "image"},
        {".gif", "image"},
        {".txt", "text"},
        {".json", "text"},
        {".conf", "text"},
    };
    bruce_result_t loaders_registered =
        app_runner__register_loaders_all(default_loaders, sizeof(default_loaders) / sizeof(default_loaders[0]));
    if (loaders_registered != BRUCE_OK) {
        ESP_LOGE(
            "boot", "app_runner__register_loaders_all() failed: %d (duplicate or malformed entry?)",
            (int)loaders_registered
        );
    }

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
