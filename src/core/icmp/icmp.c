#include "core_sdk/icmp.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include "core/network/network.h"
#include "core_sdk/permission.h"

#define ICMP__DEFAULT_TIMEOUT_MS 1000u

typedef struct {
    SemaphoreHandle_t done;
    bool got_reply;
    uint32_t round_trip_ms;
} icmp__wait_t;

/* Runs on lwIP's own internal ping task, not the caller's -- only ever
 * touches the wait_t the caller is blocked on, nothing shared beyond that. */
static void icmp__on_success(esp_ping_handle_t handle, void *context) {
    icmp__wait_t *wait = context;
    uint32_t elapsed_ms = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    wait->got_reply = true;
    wait->round_trip_ms = elapsed_ms;
}

static void icmp__on_end(esp_ping_handle_t handle, void *context) {
    icmp__wait_t *wait = context;
    /* Safe to delete the session from within its own callback -- lwIP's ping
     * example does the same; the session's internal task isn't torn down
     * until this callback returns. */
    esp_ping_delete_session(handle);
    xSemaphoreGive(wait->done);
}

bruce_result_t icmp__ping(const char *host, uint32_t timeout_ms, uint32_t *out_round_trip_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (host == NULL || host[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t network_result = network__init();
    if (network_result != BRUCE_OK) return network_result;

    struct addrinfo hints = {.ai_family = AF_INET};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, NULL, &hints, &addresses) != 0 || addresses == NULL) return BRUCE_ERR_NOT_FOUND;
    struct in_addr resolved = ((struct sockaddr_in *)addresses->ai_addr)->sin_addr;
    freeaddrinfo(addresses);

    icmp__wait_t wait = {.done = xSemaphoreCreateBinary()};
    if (wait.done == NULL) return BRUCE_ERR_NO_MEMORY;

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 1;
    config.timeout_ms = timeout_ms == 0 ? ICMP__DEFAULT_TIMEOUT_MS : timeout_ms;
    config.interval_ms = config.timeout_ms; /* irrelevant with count == 1, no second packet is ever sent */
    memset(&config.target_addr, 0, sizeof(config.target_addr));
    inet_addr_to_ip4addr(ip_2_ip4(&config.target_addr), &resolved);

    esp_ping_callbacks_t callbacks = {
        .cb_args = &wait,
        .on_ping_success = icmp__on_success,
        .on_ping_timeout = NULL,
        .on_ping_end = icmp__on_end,
    };

    esp_ping_handle_t session = NULL;
    if (esp_ping_new_session(&config, &callbacks, &session) != ESP_OK) {
        vSemaphoreDelete(wait.done);
        return BRUCE_ERR_IO;
    }
    if (esp_ping_start(session) != ESP_OK) {
        esp_ping_delete_session(session);
        vSemaphoreDelete(wait.done);
        return BRUCE_ERR_IO;
    }

    /* on_ping_end always fires exactly once, on success or timeout alike --
     * wait a little past the ping's own timeout for it to do so. */
    BaseType_t signaled = xSemaphoreTake(wait.done, pdMS_TO_TICKS(config.timeout_ms + 500u));
    vSemaphoreDelete(wait.done);
    if (signaled != pdTRUE || !wait.got_reply) return BRUCE_ERR_TIMEOUT;

    if (out_round_trip_ms != NULL) *out_round_trip_ms = wait.round_trip_ms;
    return BRUCE_OK;
}
