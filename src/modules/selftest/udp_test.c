#include "udp_test.h"

#include "core_sdk/udp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Exercises udp__open()/udp__send_to()/udp__receive_from()/udp__close()
 * (core/udp/udp.c) end to end over the loopback interface: two sockets each
 * bound to their own local port on 127.0.0.1, sending each other one
 * datagram apiece and reading it back, checking both the payload and the
 * sender address udp__receive_from() reports, plus that polling an empty
 * socket times out rather than returning stale data.
 *
 * Runs as the selftest task itself -- a built-in process, which
 * permission__check() always allows (see permission.c) -- rather than a
 * synthetic external process; permission enforcement itself is covered
 * separately by selftest__run_udp_permission_denied_case() in wifi_test.c.
 * The loopback interface (CONFIG_LWIP_NETIF_LOOPBACK) works independently of
 * any real Wi-Fi connection, so this runs the same way under QEMU as tcp__
 * exercised against a real network would elsewhere.
 */
bool selftest__run_udp_loopback_case(void) {
    const uint16_t port_a = 47001;
    const uint16_t port_b = 47002;

    bruce_udp_id_t socket_a = BRUCE_UDP_ID_INVALID;
    bruce_udp_id_t socket_b = BRUCE_UDP_ID_INVALID;
    if (udp__open(port_a, &socket_a) != BRUCE_OK || udp__open(port_b, &socket_b) != BRUCE_OK) {
        printf("[selftest] udp/loopback: open failed\n");
        udp__close(socket_a);
        udp__close(socket_b);
        return false;
    }

    static const char message_to_b[] = "hello b";
    size_t sent = 0;
    if (udp__send_to(socket_a, "127.0.0.1", port_b, message_to_b, sizeof(message_to_b), 1000, &sent) !=
            BRUCE_OK ||
        sent != sizeof(message_to_b)) {
        printf("[selftest] udp/loopback: send a->b failed\n");
        udp__close(socket_a);
        udp__close(socket_b);
        return false;
    }

    char received[32] = {0};
    size_t received_size = 0;
    bruce_udp_endpoint_t sender = {0};
    if (udp__receive_from(socket_b, received, sizeof(received), 1000, &received_size, &sender) != BRUCE_OK ||
        received_size != sizeof(message_to_b) || strcmp(received, message_to_b) != 0 ||
        strcmp(sender.host, "127.0.0.1") != 0 || sender.port != port_a) {
        printf(
            "[selftest] udp/loopback: receive a->b mismatch (\"%s\", %u bytes, from %s:%u)\n", received,
            (unsigned)received_size, sender.host, sender.port
        );
        udp__close(socket_a);
        udp__close(socket_b);
        return false;
    }

    static const char message_to_a[] = "hello a";
    if (udp__send_to(socket_b, "127.0.0.1", port_a, message_to_a, sizeof(message_to_a), 1000, &sent) !=
            BRUCE_OK ||
        sent != sizeof(message_to_a)) {
        printf("[selftest] udp/loopback: send b->a failed\n");
        udp__close(socket_a);
        udp__close(socket_b);
        return false;
    }

    memset(received, 0, sizeof(received));
    if (udp__receive_from(socket_a, received, sizeof(received), 1000, &received_size, NULL) != BRUCE_OK ||
        received_size != sizeof(message_to_a) || strcmp(received, message_to_a) != 0) {
        printf(
            "[selftest] udp/loopback: receive b->a mismatch (\"%s\", %u bytes)\n", received,
            (unsigned)received_size
        );
        udp__close(socket_a);
        udp__close(socket_b);
        return false;
    }

    /* No pending datagram on either socket now -- a zero-timeout poll must
     * time out rather than return stale or garbage data. */
    if (udp__receive_from(socket_a, received, sizeof(received), 0, &received_size, NULL) != BRUCE_ERR_TIMEOUT) {
        printf("[selftest] udp/loopback: receive on an empty socket did not time out\n");
        udp__close(socket_a);
        udp__close(socket_b);
        return false;
    }

    if (udp__close(socket_a) != BRUCE_OK || udp__close(socket_b) != BRUCE_OK) {
        printf("[selftest] udp/loopback: close failed\n");
        return false;
    }

    printf("[selftest] udp/loopback: OK\n");
    return true;
}
