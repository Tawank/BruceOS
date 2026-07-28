#include "webui_app.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/http_server.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/wifi.h"

typedef enum {
    WEBUI_APP_NETWORK_EXISTING = 0,
    WEBUI_APP_NETWORK_AP,
} webui_app_network_mode_t;

static webui_app_network_mode_t s_network_mode;

static const char s_index_html[] = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                                   "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                                   "<title>Bruce WebUI</title><style>"
                                   ":root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;min-height:"
                                   "100vh;display:grid;"
                                   "place-items:center;background:#0a0d0b;color:#e8ffe9;font:16px/1.5 "
                                   "ui-monospace,monospace}"
                                   "main{width:min(42rem,calc(100% - 2rem));border:1px solid "
                                   "#55f27b;padding:clamp(1.5rem,5vw,3rem);"
                                   "box-shadow:8px 8px 0 "
                                   "#173d21}small{color:#80b98d}code{color:#55f27b}</style></head>"
                                   "<body><main><small>BRUCE IDF / WEB SERVICE</small><h1>WebUI is "
                                   "running.</h1>"
                                   "<p>The native server foundation is online. Administrative file, input, "
                                   "and command APIs are not enabled "
                                   "in this initial security-safe slice.</p><p>Status: <code "
                                   "id=\"status\">loading</code></p></main>"
                                   "<script>const "
                                   "out=document.getElementById('status');fetch('/api/"
                                   "status').then(r=>r.json())"
                                   ".then(s=>out.textContent=`${s.mode} / "
                                   "${s.ip}:${s.port}`).catch(()=>out.textContent='unavailable')"
                                   "</script></body></html>";

static int webui_app__status(bool gui) {
    bruce_http_server_status_t status;
    bruce_result_t result = http_server__get_status(&status);
    if (result != BRUCE_OK) return result;

    char message[128];
    if (status.running) {
        const char *ip = wifi__get_ip();
        snprintf(
            message,
            sizeof(message),
            "Running on http://%s:%u\nNetwork: %s",
            ip != NULL ? ip : "unknown",
            status.port,
            s_network_mode == WEBUI_APP_NETWORK_AP ? "access point" : "existing Wi-Fi"
        );
    } else {
        snprintf(message, sizeof(message), "WebUI is stopped");
    }
    if (gui) (void)dialog__message(BRUCE_DIALOG_INFO, "WebUI", message);
    else stdio__printf("%s\n", message);
    return BRUCE_OK;
}

static int webui_app__start(webui_app_network_mode_t mode, bool gui) {
    bruce_result_t result;
    if (mode == WEBUI_APP_NETWORK_AP) {
        result = wifi__is_ap_running() ? BRUCE_OK : wifi__setup_ap();
    } else {
        result = wifi__is_connected() ? BRUCE_OK : wifi__connect_known();
    }
    if (result != BRUCE_OK) {
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not start Wi-Fi");
        else stdio__printf("Wi-Fi start failed: %d\n", result);
        return result;
    }

    const char *ip = wifi__get_ip();
    char status_json[128];
    int status_len = snprintf(
        status_json,
        sizeof(status_json),
        "{\"running\":true,\"mode\":\"%s\",\"ip\":\"%s\",\"port\":80}",
        mode == WEBUI_APP_NETWORK_AP ? "ap" : "existing",
        ip != NULL ? ip : ""
    );
    if (status_len < 0 || (size_t)status_len >= sizeof(status_json)) return BRUCE_ERR_INTERNAL;

    const bruce_http_server_route_t routes[] = {
        {
         .method = BRUCE_HTTP_SERVER_GET,
         .uri = "/",
         .content_type = "text/html; charset=utf-8",
         .body = s_index_html,
         .body_len = sizeof(s_index_html) - 1u,
         },
        {
         .method = BRUCE_HTTP_SERVER_GET,
         .uri = "/api/status",
         .content_type = "application/json",
         .body = status_json,
         .body_len = (size_t)status_len,
         },
    };
    const bruce_http_server_options_t options = {
        .port = 80,
        .routes = routes,
        .route_count = sizeof(routes) / sizeof(routes[0]),
    };
    result = http_server__start(&options);
    if (result != BRUCE_OK) {
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not start WebUI or Wi-Fi");
        else stdio__printf("WebUI start failed: %d\n", result);
        return result;
    }
    s_network_mode = mode;
    return webui_app__status(gui);
}

static int webui_app__gui(void) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Start access point", .value = "ap"    },
        {.label = "Use existing Wi-Fi", .value = "sta"   },
        {.label = "Status",             .value = "status"},
        {.label = "Stop WebUI",         .value = "stop"  },
        {.label = "Exit",               .value = "exit"  },
    };
    size_t selected = 0;
    bruce_result_t result = dialog__choice("WebUI", "Browser access", choices, 5, &selected, NULL);
    if (result == BRUCE_ERR_CANCELLED || selected == 4) return BRUCE_OK;
    if (result != BRUCE_OK) return result;
    if (selected == 0) return webui_app__start(WEBUI_APP_NETWORK_AP, true);
    if (selected == 1) return webui_app__start(WEBUI_APP_NETWORK_EXISTING, true);
    if (selected == 2) return webui_app__status(true);
    result = http_server__stop();
    if (result == BRUCE_OK) (void)dialog__message(BRUCE_DIALOG_SUCCESS, "WebUI", "WebUI stopped");
    return result;
}

static void webui_app__usage(void) {
    stdio__printf("WebUI commands:\n");
    stdio__printf("  webui start ap|sta\n");
    stdio__printf("  webui status\n");
    stdio__printf("  webui stop\n");
}

int webui_app_main(int argc, char **argv) {
    if (app_runner__args_have_gui(argc, argv)) return webui_app__gui();
    if (argc == 0 || argv == NULL || argv[0] == NULL || strcmp(argv[0], "status") == 0) {
        return webui_app__status(false);
    }
    if (strcmp(argv[0], "stop") == 0) {
        bruce_result_t result = http_server__stop();
        if (result == BRUCE_OK) stdio__printf("WebUI stopped\n");
        return result;
    }
    if (strcmp(argv[0], "start") == 0) {
        if (strcmp(argv[1], "ap") == 0) {
            return webui_app__start(WEBUI_APP_NETWORK_AP, false);
        } else {
            return webui_app__start(WEBUI_APP_NETWORK_EXISTING, false);
        }
    }
    webui_app__usage();
    return strcmp(argv[0], "help") == 0 ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}
