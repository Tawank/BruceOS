#include "ir_test.h"

#include <stdio.h>
#include <string.h>

#include "core/permission/permission.h"
#include "core/task/task.h"
#include "core_sdk/ir.h"
#include "core_sdk/permission.h"
#include "core_sdk/task.h"

static volatile bruce_result_t s_ir_result;
static volatile bool s_ir_ran;

static int selftest__ir_external_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    s_ir_result = ir__transmit("20DF10EF", "NEC", 32, 0);
    s_ir_ran = true;
    return 0;
}

bool selftest__run_ir_permission_denied_case(void)
{
    permission__test_reset();
    permission__set("ir_denied.js", BRUCE_PERMISSION_IR, false);
    s_ir_result = BRUCE_OK;
    s_ir_ran = false;
    task_create_params_t params = {
        .name = "selftest_ir",
        .entry = selftest__ir_external_entry,
        .built_in = false,
        .permission_key = "ir_denied.js",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_task_id_t id = BRUCE_TASK_ID_INVALID;
    if (task_registry__create(&params, &id) != BRUCE_OK) return false;
    bruce_result_t wait = task__wait(id, 5000);
    bool ok = (wait == BRUCE_OK || wait == BRUCE_ERR_NOT_FOUND) && s_ir_ran &&
              s_ir_result == BRUCE_ERR_PERMISSION;
    printf("[selftest] ir/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", s_ir_result);
    return ok;
}

bool selftest__run_ir_validation_case(void)
{
    uint32_t timing = 560;
    bool ok = ir__transmit(NULL, "NEC", 32, 0) == BRUCE_ERR_INVALID_ARGUMENT &&
              ir__transmit("1", "unknown", 1, 0) == BRUCE_ERR_UNSUPPORTED &&
              ir__transmit_raw(&timing, 1, BRUCE_IR_DEFAULT_FREQUENCY_HZ, 0) == BRUCE_ERR_INVALID_ARGUMENT &&
              ir__receive(false, 0, NULL, 0) == BRUCE_ERR_INVALID_ARGUMENT &&
              ir__transmit_record(NULL, 0) == BRUCE_ERR_INVALID_ARGUMENT &&
              ir__transmit_record("Filetype: IR signals file\nVersion: 1\n#\n", 0) == BRUCE_ERR_NOT_FOUND &&
              ir__transmit_file("relative.ir", 0) == BRUCE_ERR_INVALID_PATH;
    printf("[selftest] ir/validation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
