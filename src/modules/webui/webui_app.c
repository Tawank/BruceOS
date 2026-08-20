#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/dialog.h"
#include "core_sdk/http_server.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/status_icon.h"
#include "core_sdk/stdio.h"
#include "core_sdk/wifi.h"
#include "embedded_resources.h"
#include "webui_internal.h"

static webui_app_network_mode_t s_network_mode;

#define WEBUI_STATUS_ICON_KEY "module.webui"

webui_app_network_mode_t webui__get_network_mode(void) { return s_network_mode; }

static const webui_asset_t s_index_asset = {
    web_interface_index_html_gz, web_interface_index_html_gz_size, "text/html; charset=utf-8"
};
static const webui_asset_t s_login_asset = {
    web_interface_login_html_gz, web_interface_login_html_gz_size, "text/html; charset=utf-8"
};
static const webui_asset_t s_css_asset = {
    web_interface_index_css_gz, web_interface_index_css_gz_size, "text/css; charset=utf-8"
};
static const webui_asset_t s_js_asset = {
    web_interface_index_js_gz, web_interface_index_js_gz_size, "text/javascript; charset=utf-8"
};

static bruce_result_t webui__root(bruce_http_server_request_t *request, void *context) {
    (void)context;
    return webui__serve_asset(request, webui__authenticated(request) ? &s_index_asset : &s_login_asset);
}

static bruce_result_t webui__asset(bruce_http_server_request_t *request, void *context) {
    return webui__serve_asset(request, context);
}

static const bruce_http_server_route_t s_routes[] = {
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/", .callback = webui__root},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/login", .callback = webui__login},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/logout", .callback = webui__logout},
    {.method = BRUCE_HTTP_SERVER_GET,
     .uri = "/index.css",
     .callback = webui__asset,
     .context = (void *)&s_css_asset},
    {.method = BRUCE_HTTP_SERVER_GET,
     .uri = "/index.js",
     .callback = webui__asset,
     .context = (void *)&s_js_asset},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/theme.css", .callback = webui__theme},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/systeminfo", .callback = webui__system_info},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/listfiles", .callback = webui__list_files},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/file", .callback = webui__file},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/rename", .callback = webui__rename},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/edit", .callback = webui__edit},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/upload", .callback = webui__upload},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/cm", .callback = webui__command},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/wifi", .callback = webui__wifi},
    {.method = BRUCE_HTTP_SERVER_POST, .uri = "/wifi", .callback = webui__wifi},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/getscreen", .callback = webui__screen},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/reboot", .callback = webui__reboot},
    {.method = BRUCE_HTTP_SERVER_GET, .uri = "/api/status", .callback = webui__api_status},
};

static int webui_app__status(bool gui) {
    bruce_http_server_status_t status;
    bruce_result_t result = http_server__get_status(&status);
    if (result != BRUCE_OK) return result;
    char message[160];
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
    } else snprintf(message, sizeof(message), "WebUI is stopped");
    if (gui) (void)dialog__message(BRUCE_DIALOG_INFO, "WebUI", message);
    else stdio__printf("%s\n", message);
    return BRUCE_OK;
}

static bruce_result_t webui_app__stop(void) {
    bruce_result_t result = http_server__stop();
    if (result == BRUCE_OK) (void)status_icon__remove(WEBUI_STATUS_ICON_KEY);
    return result;
}

static int webui_app__start(webui_app_network_mode_t mode, bool gui) {
    bruce_result_t result;
    if (mode == WEBUI_APP_NETWORK_AP) result = wifi__is_ap_running() ? BRUCE_OK : wifi__setup_ap();
    else result = wifi__is_connected() ? BRUCE_OK : wifi__connect_known();
    if (result != BRUCE_OK) {
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not start Wi-Fi");
        else stdio__printf("Wi-Fi start failed: %d\n", result);
        return result;
    }
    const bruce_http_server_options_t options = {
        .port = 80, .routes = s_routes, .route_count = sizeof(s_routes) / sizeof(s_routes[0])
    };
    s_network_mode = mode;
    result = http_server__start(&options);
    if (result != BRUCE_OK) {
        if (gui) (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not start WebUI");
        else stdio__printf("WebUI start failed: %d\n", result);
        return result;
    }
    (void)status_icon__push_named(WEBUI_STATUS_ICON_KEY, "web");
    return webui_app__status(gui);
}

static int webui_app__gui(void) {
    for (;;) {
        bool running = http_server__is_running();
        const bruce_dialog_choice_t choices[] = {
            {.label = running ? "Stop server" : "Start server", .value = "toggle"   },
            {.label = "Exit",                                   .value = "exit"     },
            {.label = "Stop and exit",                          .value = "stop-exit"},
        };
        size_t selected = 0;
        bruce_result_t result = dialog__choice_launcher("WebUI", NULL, choices, 3, &selected);
        if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
        if (result != BRUCE_OK) return result;
        if (strcmp(choices[selected].value, "exit") == 0) return BRUCE_OK;
        if (strcmp(choices[selected].value, "stop-exit") == 0) {
            if (http_server__is_running()) return webui_app__stop();
            return BRUCE_OK;
        }
        if (running) {
            result = webui_app__stop();
            if (result != BRUCE_OK)
                (void)dialog__message(BRUCE_DIALOG_ERROR, "WebUI", "Could not stop server");
            continue;
        }
        if (wifi__is_connected()) {
            (void)webui_app__start(WEBUI_APP_NETWORK_EXISTING, true);
            continue;
        }
        if (wifi__is_ap_running()) {
            (void)webui_app__start(WEBUI_APP_NETWORK_AP, true);
            continue;
        }
        if (wifi__connect_known() == BRUCE_OK) {
            (void)webui_app__start(WEBUI_APP_NETWORK_EXISTING, true);
            continue;
        }
        const bruce_dialog_choice_t network_choices[] = {
            {.label = "Start access point", .value = "ap"    },
            {.label = "Cancel",             .value = "cancel"},
        };
        size_t network = 0;
        result = dialog__choice_launcher(
            "Start WebUI", "Existing Wi-Fi unavailable", network_choices, 2, &network
        );
        if (result == BRUCE_ERR_CANCELLED ||
            (result == BRUCE_OK && strcmp(network_choices[network].value, "cancel") == 0))
            continue;
        if (result != BRUCE_OK) return result;
        (void)webui_app__start(WEBUI_APP_NETWORK_AP, true);
    }
}

static void webui_app__print_help(void) { stdio__printf("Usage: webui [status|stop|start ap|start sta]\n"); }

int webui_app_main(int argc, char **argv) {
    if (runtime__gui_requested()) { return webui_app__gui(); }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        webui_app__print_help();
        return BRUCE_OK;
    }
    if (argc <= 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) return webui_app__status(false);
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        bruce_result_t result = webui_app__stop();
        if (result == BRUCE_OK) stdio__printf("WebUI stopped\n");
        return result;
    }
    if (argc == 3 && strcmp(argv[1], "start") == 0) {
        if (strcmp(argv[2], "ap") == 0) return webui_app__start(WEBUI_APP_NETWORK_AP, false);
        if (strcmp(argv[2], "sta") == 0) return webui_app__start(WEBUI_APP_NETWORK_EXISTING, false);
    }
    webui_app__print_help();
    return BRUCE_ERR_INVALID_ARGUMENT;
}
