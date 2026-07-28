#include "bnu_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/result.h"
#include "modules/bnu/bnu_app.h"

bool selftest__run_bnu_case(void) {
    char *cd_root_argv[] = {"cd"};
    char *cd_dot_argv[] = {"cd", "."};
    char *pwd_argv[] = {"pwd"};
    char *ls_argv[] = {"ls"};
    char *free_argv[] = {"free"};
    char *top_argv[] = {"top"};
    bool ok = bnu_cd_app_main(1, cd_root_argv) == BRUCE_OK &&
              bnu_cd_app_main(2, cd_dot_argv) == BRUCE_OK &&
              strcmp(bnu__get_working_directory(), "/") == 0 &&
              bnu_pwd_app_main(1, pwd_argv) == BRUCE_OK && bnu_ls_app_main(1, ls_argv) == BRUCE_OK &&
              bnu_free_app_main(1, free_argv) == BRUCE_OK && bnu_top_app_main(1, top_argv) == BRUCE_OK;
    printf("[selftest] bnu: %s\n", ok ? "OK" : "failed");
    return ok;
}
