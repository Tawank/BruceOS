#include "net_test.h"

#include "core_sdk/net.h"

#include <stdio.h>
#include <string.h>

bool selftest__run_net_ipv4_case(void) {
    uint32_t address = 0;
    bool roundtrip = net__parse_ipv4("192.168.1.42", &address) == BRUCE_OK && address == 0xC0A8012Au;

    char formatted[BRUCE_NET_IPV4_TEXT_MAX];
    net__format_ipv4(address, formatted, sizeof(formatted));
    bool format_ok = strcmp(formatted, "192.168.1.42") == 0;

    bool zero_ok = net__parse_ipv4("0.0.0.0", &address) == BRUCE_OK && address == 0;
    bool max_ok = net__parse_ipv4("255.255.255.255", &address) == BRUCE_OK && address == 0xFFFFFFFFu;

    bool octet_range_rejected = net__parse_ipv4("192.168.1.256", &address) != BRUCE_OK;
    bool too_few_rejected = net__parse_ipv4("192.168.1", &address) != BRUCE_OK;
    bool too_many_rejected = net__parse_ipv4("192.168.1.1.1", &address) != BRUCE_OK;
    bool trailing_garbage_rejected = net__parse_ipv4("192.168.1.1x", &address) != BRUCE_OK;
    bool hostname_rejected = net__parse_ipv4("example.com", &address) != BRUCE_OK;
    bool null_rejected = net__parse_ipv4(NULL, &address) != BRUCE_OK;

    bool ok = roundtrip && format_ok && zero_ok && max_ok && octet_range_rejected && too_few_rejected &&
              too_many_rejected && trailing_garbage_rejected && hostname_rejected && null_rejected;
    printf(
        "[selftest] net/ipv4: %s (roundtrip=%d format=%d zero=%d max=%d octet_range=%d too_few=%d "
        "too_many=%d trailing=%d hostname=%d null=%d)\n",
        ok ? "OK" : "FAIL", roundtrip, format_ok, zero_ok, max_ok, octet_range_rejected, too_few_rejected,
        too_many_rejected, trailing_garbage_rejected, hostname_rejected, null_rejected
    );
    return ok;
}
