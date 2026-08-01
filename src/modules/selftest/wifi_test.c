/* A5 Wi-Fi / HTTP vertical-slice permission coverage.
 *
 * These tests verify that the migrated Wi-Fi Core API (`wifi__scan()`) and the
 * new HTTP Core API (`http__request()`) enforce their respective `wifi` and
 * `http` permissions independently.  They do so by running the actual public
 * APIs from a synthetic external process, the same way a real ELF/JS app would.
 *
 * The denied cases return before any hardware/network activity because the
 * permission check is the first thing each API does. */
#include <stdio.h>
#include <string.h>

#include "core/permission/permission.h"
#include "core/process/process.h"
#include "core_sdk/http.h"
#include "core_sdk/permission.h"
#include "core_sdk/result.h"
#include "core_sdk/ssh.h"
#include "core_sdk/process.h"
#include "core_sdk/tcp.h"
#include "core_sdk/wifi.h"

#include "wifi_test.h"

typedef struct {
    volatile bruce_result_t result;
    volatile bool ran;
} selftest__wifi_http_result_t;

static selftest__wifi_http_result_t s_wifi_http_result;

static int selftest__wifi_scan_entry(int argc, char **argv) {
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

static int selftest__http_request_entry(int argc, char **argv) {
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

static int selftest__tcp_connect_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bruce_tcp_id_t socket = BRUCE_TCP_ID_INVALID;
    s_wifi_http_result.result = tcp__connect("127.0.0.1", 1, 1, &socket);
    s_wifi_http_result.ran = true;
    return 0;
}

static int selftest__ssh_connect_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bruce_ssh_id_t session = BRUCE_SSH_ID_INVALID;
    s_wifi_http_result.result = ssh__connect("127.0.0.1", 22, 1, &session);
    s_wifi_http_result.ran = true;
    return 0;
}

static bruce_result_t selftest__run_as_external(const char *permission_key, bruce_app_entry_t entry) {
    memset(&s_wifi_http_result, 0, sizeof(s_wifi_http_result));
    process_create_params_t params = {
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
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) { return BRUCE_ERR_INTERNAL; }
    bruce_result_t wait_result = process__wait(id, 5000);
    if (wait_result != BRUCE_OK && wait_result != BRUCE_ERR_NOT_FOUND) { return BRUCE_ERR_TIMEOUT; }
    return s_wifi_http_result.ran ? s_wifi_http_result.result : BRUCE_ERR_INTERNAL;
}

static bool selftest__denied_wifi_scan(void) {
    permission__test_reset();
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_WIFI, false);
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_HTTP, false);

    bruce_result_t result = selftest__run_as_external("wifi_http_indep.elf", selftest__wifi_scan_entry);

    bool ok = result == BRUCE_ERR_PERMISSION;
    printf("[selftest] wifi/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", result);
    return ok;
}

static bool selftest__denied_http_request(void) {
    permission__test_reset();
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_WIFI, false);
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_HTTP, false);

    bruce_result_t result = selftest__run_as_external("wifi_http_indep.elf", selftest__http_request_entry);

    bool ok = result == BRUCE_ERR_PERMISSION;
    printf("[selftest] http/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", result);
    return ok;
}

bool selftest__run_wifi_permission_denied_case(void) { return selftest__denied_wifi_scan(); }

bool selftest__run_http_permission_denied_case(void) { return selftest__denied_http_request(); }

bool selftest__run_wifi_http_independent_permission_case(void) {
    permission__test_reset();
    /* Deny Wi-Fi, allow HTTP. wifi__scan must fail with permission error while
     * http__request must not fail because of the Wi-Fi permission. */
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_WIFI, false);
    permission__set("wifi_http_indep.elf", BRUCE_PERMISSION_HTTP, true);

    bruce_result_t wifi_result = selftest__run_as_external("wifi_http_indep.elf", selftest__wifi_scan_entry);

    /* For the allowed HTTP case, the network request itself will almost
     * certainly fail (no connectivity in a self-test environment), but it must
     * not be BRUCE_ERR_PERMISSION because `http` was allowed. */
    bruce_result_t http_result =
        selftest__run_as_external("wifi_http_indep.elf", selftest__http_request_entry);

    bool ok = wifi_result == BRUCE_ERR_PERMISSION && http_result != BRUCE_ERR_PERMISSION;
    printf(
        "[selftest] wifi+http/independent-permission: %s (wifi=%d http=%d)\n",
        ok ? "OK" : "FAIL",
        wifi_result,
        http_result
    );
    return ok;
}

bool selftest__run_tcp_permission_denied_case(void) {
    permission__test_reset();
    permission__set("tcp_denied.elf", BRUCE_PERMISSION_WIFI, false);
    bruce_result_t result = selftest__run_as_external("tcp_denied.elf", selftest__tcp_connect_entry);
    bool ok = result == BRUCE_ERR_PERMISSION;
    printf("[selftest] tcp/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", result);
    return ok;
}

bool selftest__run_ssh_permission_denied_case(void) {
    permission__test_reset();
    permission__set("ssh_denied.elf", BRUCE_PERMISSION_SSH, false);
    bruce_result_t result = selftest__run_as_external("ssh_denied.elf", selftest__ssh_connect_entry);
    bool ok = result == BRUCE_ERR_PERMISSION;
    printf("[selftest] ssh/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", result);
    return ok;
}

bool selftest__run_ssh_keygen_case(void) {
    char private_key[BRUCE_SSH_PRIVATE_KEY_MAX_SIZE];
    char public_key[BRUCE_SSH_PUBLIC_KEY_MAX_SIZE];
    size_t private_size = 0;
    size_t public_size = 0;
    bruce_result_t result = ssh__generate_keypair_ex(
        BRUCE_SSH_KEY_ECDSA_P256,
        private_key, sizeof(private_key), &private_size, public_key, sizeof(public_key), &public_size
    );
    bool ok = result == BRUCE_OK && private_size > 0 && public_size > 0 &&
              strncmp(private_key, "-----BEGIN EC PRIVATE KEY-----\n", 31) == 0 &&
              strncmp(public_key, "ecdsa-sha2-nistp256 ", 20) == 0;
    memset(private_key, 0, sizeof(private_key));
    private_size = 0;
    public_size = 0;
    bruce_result_t ed25519_result = ssh__generate_keypair_ex(
        BRUCE_SSH_KEY_ED25519, private_key, sizeof(private_key), &private_size,
        public_key, sizeof(public_key), &public_size
    );
    bool ed25519_ok = ed25519_result == BRUCE_OK && private_size > 0 && public_size > 0 &&
                      strncmp(private_key, "-----BEGIN OPENSSH PRIVATE KEY-----\n", 36) == 0 &&
                      strncmp(public_key, "ssh-ed25519 ", 12) == 0;
    ok = ok && ed25519_ok;
    memset(private_key, 0, sizeof(private_key));
    printf(
        "[selftest] ssh/keygen: %s (ecdsa=%d ed25519=%d private=%u public=%u)\n",
        ok ? "OK" : "FAIL",
        result,
        ed25519_result,
        (unsigned)private_size,
        (unsigned)public_size
    );
    return ok;
}
