#include "nrf24_test.h"

#include <stdio.h>

#include "core/permission/permission.h"
#include "core/task/task.h"
#include "core_sdk/nrf24.h"
#include "core_sdk/permission.h"
#include "core_sdk/task.h"

static volatile bruce_result_t s_nrf24_result;
static volatile bool s_nrf24_ran;

static int selftest__nrf24_external_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bool connected = false;
    s_nrf24_result = nrf24__probe(&connected);
    s_nrf24_ran = true;
    return 0;
}

bool selftest__run_nrf24_permission_denied_case(void)
{
    permission__test_reset();
    permission__set("nrf24_denied.js", BRUCE_PERMISSION_RF, false);
    s_nrf24_result = BRUCE_OK;
    s_nrf24_ran = false;
    task_create_params_t params = {
        .name = "selftest_nrf24",
        .entry = selftest__nrf24_external_entry,
        .built_in = false,
        .permission_key = "nrf24_denied.js",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_task_id_t id = BRUCE_TASK_ID_INVALID;
    if (task_registry__create(&params, &id) != BRUCE_OK) return false;
    bruce_result_t wait = task__wait(id, 5000);
    bool ok = (wait == BRUCE_OK || wait == BRUCE_ERR_NOT_FOUND) && s_nrf24_ran &&
              s_nrf24_result == BRUCE_ERR_PERMISSION;
    printf("[selftest] nrf24/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", s_nrf24_result);
    return ok;
}

bool selftest__run_nrf24_validation_case(void)
{
    uint8_t activity = 0;
    bool ok = nrf24__probe(NULL) == BRUCE_ERR_INVALID_ARGUMENT &&
              nrf24__set_channel(126) == BRUCE_ERR_INVALID_ARGUMENT &&
              nrf24__get_channel(NULL) == BRUCE_ERR_INVALID_ARGUMENT &&
              nrf24__scan(0, 0, 1, &activity) == BRUCE_ERR_INVALID_ARGUMENT &&
              nrf24__scan(125, 2, 1, &activity) == BRUCE_ERR_INVALID_ARGUMENT &&
              nrf24__scan(0, 1, 0, &activity) == BRUCE_ERR_INVALID_ARGUMENT &&
              nrf24__scan(0, 1, 1, NULL) == BRUCE_ERR_INVALID_ARGUMENT;
    printf("[selftest] nrf24/validation: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
