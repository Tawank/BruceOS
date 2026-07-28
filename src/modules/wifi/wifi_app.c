#include "wifi_app.h"

#include <stdbool.h>
#include <string.h>

#include "core_sdk/stdio.h"
#include "core_sdk/wifi.h"

static int wifi_app_default(void) { return wifi__connect_known() == BRUCE_OK ? 0 : -1; }

static int wifi_app_scan(void) {
    wifi__network_t networks[32];
    int count = wifi__scan(networks, sizeof(networks) / sizeof(networks[0]));
    if (count < 0) { return -1; }

    for (int i = 0; i < count; ++i) {
        stdio__printf(
            "%s rssi=%d channel=%u auth=%u\n",
            networks[i].ssid,
            networks[i].rssi,
            networks[i].channel,
            networks[i].authmode
        );
    }
    return 0;
}

static int wifi_app_connect(int argc, char **argv) {
    if (argc == 2) { return wifi__connect_known() == BRUCE_OK ? 0 : -1; }
    if (argc < 4) {
        stdio__printf("wifi connect requires ssid and password\n");
        return -1;
    }

    if (wifi__connect(argv[2], argv[3], 10000) != BRUCE_OK) { return -1; }
    return wifi__add_credential(argv[2], argv[3]) == BRUCE_OK ? 0 : -1;
}

static int wifi_app_ap(int argc, char **argv) {
    if (argc < 3) {
        stdio__printf("wifi ap requires start or info\n");
        return -1;
    }
    if (strcmp(argv[2], "start") == 0) { return wifi__setup_ap() == BRUCE_OK ? 0 : -1; }
    if (strcmp(argv[2], "info") == 0) {
        if (!wifi__is_ap_running()) {
            stdio__printf("Wi-Fi AP is not running\n");
            return -1;
        }
        const char *ssid = wifi__get_ssid();
        const char *ip = wifi__get_ip();
        stdio__printf("SSID: %s\nIP: %s\n", ssid != NULL ? ssid : "unknown", ip != NULL ? ip : "unknown");
        return 0;
    }
    stdio__printf("Unknown wifi ap command: %s\n", argv[2]);
    return -1;
}

static int wifi_app_add(int argc, char **argv) {
    if (argc < 4) {
        stdio__printf("wifi add requires ssid and password\n");
        return -1;
    }
    return wifi__add_credential(argv[2], argv[3]) == BRUCE_OK ? 0 : -1;
}

static int wifi_app_webui(int argc, char **argv) {
    bool no_ap = false;

    if (argc > 2 && argv[2] != NULL && strcmp(argv[2], "noAp") == 0) { no_ap = true; }

    (void)no_ap;
    stdio__printf("wifi webui is unavailable\n");
    return -1;
}

int wifi_app_main(int argc, char **argv) {
    if (argc <= 1 || argv == NULL || argv[1] == NULL) { return wifi_app_default(); }

    if (strcmp(argv[1], "on") == 0) {
        return wifi__connect_known() == BRUCE_OK ? 0 : wifi__setup_ap() == BRUCE_OK ? 0 : -1;
    }

    if (strcmp(argv[1], "off") == 0) { return wifi__disconnect() == BRUCE_OK ? 0 : -1; }

    if (strcmp(argv[1], "add") == 0) { return wifi_app_add(argc, argv); }

    if (strcmp(argv[1], "ap") == 0) { return wifi_app_ap(argc, argv); }

    if (strcmp(argv[1], "webui") == 0) { return wifi_app_webui(argc, argv); }

    if (strcmp(argv[1], "arp") == 0) { return wifi__scan_hosts() == BRUCE_OK ? 0 : -1; }

    if (strcmp(argv[1], "listen") == 0) { return wifi__listen_tcp() == BRUCE_OK ? 0 : -1; }

    if (strcmp(argv[1], "sniffer") == 0) { return wifi__start_sniffer() == BRUCE_OK ? 0 : -1; }

    if (strcmp(argv[1], "scan") == 0) { return wifi_app_scan(); }

    if (strcmp(argv[1], "connect") == 0) { return wifi_app_connect(argc, argv); }

    if (strcmp(argv[1], "disconnect") == 0) { return wifi__disconnect() == BRUCE_OK ? 0 : -1; }

    stdio__printf("Unknown wifi command: %s\n", argv[1]);
    return -1;
}
