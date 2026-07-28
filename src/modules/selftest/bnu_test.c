#include "bnu_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/result.h"
#include "modules/bnu/bnu_app.h"

bool selftest__run_bnu_case(void) {
    char *dot_argv[] = {"."};
    bool ok = bnu_cd_app_main(0, NULL) == BRUCE_OK && bnu_cd_app_main(1, dot_argv) == BRUCE_OK &&
              strcmp(bnu__get_working_directory(), "/") == 0 && bnu_pwd_app_main(0, NULL) == BRUCE_OK &&
              bnu_ls_app_main(0, NULL) == BRUCE_OK && bnu_free_app_main(0, NULL) == BRUCE_OK &&
              bnu_top_app_main(0, NULL) == BRUCE_OK;
    printf("[selftest] bnu: %s\n", ok ? "OK" : "failed");
    return ok;
}
