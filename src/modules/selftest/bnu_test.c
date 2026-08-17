#include "bnu_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/environment.h"
#include "core_sdk/result.h"
#include "core_sdk/disk.h"
#include "core_sdk/storage.h"
#include "core/storage/storage.h"
#include "modules/bnu/bnu_app.h"

bool selftest__run_bnu_case(void) {
    (void)environment__unset("PWD");
    char *pwd_argv[] = {"pwd"};
    char *ls_argv[] = {"ls"};
    char *lsblk_argv[] = {"lsblk"};
    char *mount_argv[] = {"mount"};
    char *unmount_argv[] = {"unmount", "missing"};
    char *free_argv[] = {"free"};
    char *top_argv[] = {"top"};
    char *shutdown_invalid_argv[] = {"shutdown", "later"};
    char *reboot_invalid_argv[] = {"reboot", "later"};
    char *cat_argv[] = {"cat", "/selftest_bnu_cat.txt"};
    char *stty_argv[] = {"stty"};
    static const char cat_text[] = "bnu cat selftest\n";
    bruce_file_id_t cat_file = BRUCE_FILE_ID_INVALID;
    size_t cat_written = 0;
    bruce_result_t cat_open = storage__open(
        cat_argv[1], BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &cat_file
    );
    bruce_result_t cat_write = cat_open == BRUCE_OK
                                   ? storage__write(cat_file, cat_text, sizeof(cat_text) - 1, &cat_written)
                                   : cat_open;
    bruce_result_t cat_close = cat_open == BRUCE_OK ? storage__close(cat_file) : cat_open;
    size_t disk_count = 0;
    bool ok = disk__list(NULL, 0, &disk_count) == BRUCE_OK && disk_count > 1 &&
               strcmp(bnu__get_working_directory(), "/") == 0 &&
              bnu_pwd_app_main(1, pwd_argv) == BRUCE_OK && bnu_ls_app_main(1, ls_argv) == BRUCE_OK &&
              bnu_lsblk_app_main(1, lsblk_argv) == BRUCE_OK &&
              bnu_mount_app_main(1, mount_argv) == BRUCE_OK &&
               bnu_unmount_app_main(2, unmount_argv) == BRUCE_ERR_NOT_FOUND &&
               bnu_free_app_main(1, free_argv) == BRUCE_OK && bnu_top_app_main(1, top_argv) == BRUCE_OK &&
               bnu_shutdown_app_main(1, shutdown_invalid_argv) == BRUCE_ERR_INVALID_ARGUMENT &&
               bnu_shutdown_app_main(2, shutdown_invalid_argv) == BRUCE_ERR_INVALID_ARGUMENT &&
               bnu_reboot_app_main(2, reboot_invalid_argv) == BRUCE_ERR_INVALID_ARGUMENT &&
               cat_open == BRUCE_OK && cat_write == BRUCE_OK && cat_written == sizeof(cat_text) - 1 &&
              cat_close == BRUCE_OK && bnu_cat_app_main(2, cat_argv) == BRUCE_OK &&
               /* Selftest runs with no routed stdio session, so stty correctly reports "not a tty". */
               bnu_stty_app_main(1, stty_argv) == BRUCE_ERR_NOT_FOUND;
    storage__remove(cat_argv[1]);
    printf("[selftest] bnu: %s\n", ok ? "OK" : "failed");
    return ok;
}
