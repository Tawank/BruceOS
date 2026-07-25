/* A5 Wi-Fi / HTTP vertical-slice permission coverage.
 *
 * These tests verify that the migrated Wi-Fi Core API (`wifi__scan()`) and the
 * new HTTP Core API (`http__request()`) enforce their respective `wifi` and
 * `http` permissions independently.  They do so by running the actual public
 * APIs from a synthetic external task, the same way a real ELF/JS app would.
 *
 * The denied cases return before any hardware/network activity because the
 * permission check is the first thing each API does. */
#include <stdio.h>
#include <string.h>

#include "core/permission/permission.h"
#include "core/task/task.h"
#include "core_sdk/http.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"
#include "core_sdk/wifi.h"

#include "wifi_test.h"

typedef struct {
    volatile bruce_result_t result;
    volatile bool ran;
} selftest__wifi_http_result_t;

static selftest__wifi_http_result_t s_wifi_http_result;

static int selftest__wifi_scan_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi__network_t networks[4];
    int count = wifi__scan(networks, sizeof(networks) / sizeof(networks[0]));
    if (count < 0) {
        s_wifi_http_result.result = (bruce_result_t)count;
    } else {
        s_wifi_http_result.result = BRUCE_OK;
    }
    s_wifi_http_result.ran = true;
    return 0;
}

static int selftest__http_request_entry(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bruce_http_request_t request = {
        .url = "http://localhost/",
        .method = "GET",
        .body = NULL,
        .body_len = 0,
        .headers = NULL,
        .header_count = 0,
        .timeout_ms = 1000,
    };
    bruce_http_response_t response = {0};
    s_wifi_http_result.result = http__request(&request, &response);
    s_wifi_http_result.ran = true;
    return 0;
}

static bruce_result_t selftest__run_as_external(const char *permission_key, bruce_app_entry_t entry)
{
    memset(&s_wifi_http_result, 0, sizeof(s_wifi_http_result));
    task_create_params_t params = {
        .name = "selftest_wifi_http",
        .entry = entry,
        .argc = 0,
        .argv = NULL,
        .built_in = false,
        .gui_requested = false,
        .permission_key = permission_key,
        .start_in_background = true,
        .stack_bytes = 8192,
    };
    bruce_task_id_t id = BRUCE_TASK_ID_INVALID;
    if (task_registry__create(&params, &id) != BRUCE_OK) {
        return BRUCE_ERR_INTERNAL;
    }
    bruce_result_t wait_result = task__wait(id, 5000);
    if (wait_result != BRUCE_OK && wait_result != BRUCE_ERR_NOT_FOUND) {
        return BRUCE_ERR_TIMEOUT;
    }
    return s_wifi_http_result.ran ? s_wifi_http_result.result : BRUCE_ERR_INTERNAL;
}

static bool selftest__denied_wifi_scan(void)
{
    permission__test_reset();
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_WIFI, false);
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_HTTP, false);

    bruce_result_t result = selftest__run_as_external("wifi_http_indep.elf", selftest__wifi_scan_entry);

    bool ok = result == BRUCE_ERR_PERMISSION;
    printf("[selftest] wifi/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", result);
    return ok;
}

static bool selftest__denied_http_request(void)
{
    permission__test_reset();
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_WIFI, false);
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_HTTP, false);

    bruce_result_t result = selftest__run_as_external("wifi_http_indep.elf", selftest__http_request_entry);

    bool ok = result == BRUCE_ERR_PERMISSION;
    printf("[selftest] http/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", result);
    return ok;
}

bool selftest__run_wifi_permission_denied_case(void)
{
    return selftest__denied_wifi_scan();
}

bool selftest__run_http_permission_denied_case(void)
{
    return selftest__denied_http_request();
}

bool selftest__run_wifi_http_independent_permission_case(void)
{
    permission__test_reset();
    /* Deny Wi-Fi, allow HTTP. wifi__scan must fail with permission error while
     * http__request must not fail because of the Wi-Fi permission. */
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_WIFI, false);
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_HTTP, true);

    bruce_result_t wifi_result = selftest__run_as_external("wifi_http_indep.elf", selftest__wifi_scan_entry);

    /* For the allowed HTTP case, the network request itself will almost
     * certainly fail (no connectivity in a self-test environment), but it must
     * not be BRUCE_ERR_PERMISSION because `http` was allowed. */
    bruce_result_t http_result = selftest__run_as_external("wifi_http_indep.elf", selftest__http_request_entry);

    bool ok = wifi_result == BRUCE_ERR_PERMISSION && http_result != BRUCE_ERR_PERMISSION;
    printf("[selftest] wifi+http/independent-permission: %s (wifi=%d http=%d)\n", ok ? "OK" : "FAIL", wifi_result,
           http_result);
    return ok;
}
