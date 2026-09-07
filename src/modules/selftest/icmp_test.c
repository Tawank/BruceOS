#include "icmp_test.h"

#include "core_sdk/icmp.h"

#include <stdio.h>

/*
 * Exercises icmp__ping() (core/icmp/icmp.c) against the loopback interface,
 * the same way selftest__run_udp_loopback_case() exercises core/udp/udp.c --
 * CONFIG_LWIP_NETIF_LOOPBACK answers ICMP echo requests to 127.0.0.1
 * regardless of any real Wi-Fi connection, so this runs the same under QEMU
 * as pinging a real host elsewhere would.
 *
 * Runs as the selftest task itself -- a built-in process, which
 * permission__check() always allows -- rather than a synthetic external
 * process; permission enforcement itself is covered separately by
 * selftest__run_icmp_permission_denied_case() in wifi_test.c.
 */
bool selftest__run_icmp_loopback_case(void) {
    uint32_t round_trip_ms = 0xFFFFFFFFu;
    bruce_result_t reply_result = icmp__ping("127.0.0.1", 1000, &round_trip_ms);
    bool reply_ok = reply_result == BRUCE_OK && round_trip_ms != 0xFFFFFFFFu;

    /* A host with no route at all (this build has no default gateway
     * outside loopback) must not be reported as a reply, whether that
     * surfaces as an outright timeout or a quicker routing failure --
     * either way it must never be BRUCE_OK. */
    bruce_result_t unreachable_result = icmp__ping("192.0.2.1", 200, NULL);
    bool unreachable_ok = unreachable_result != BRUCE_OK;

    bool ok = reply_ok && unreachable_ok;
    printf(
        "[selftest] icmp/loopback: %s (reply=%d round_trip_ms=%u unreachable=%d)\n", ok ? "OK" : "FAIL",
        reply_result, (unsigned int)round_trip_ms, unreachable_result
    );
    return ok;
}
