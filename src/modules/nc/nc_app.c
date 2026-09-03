#include "nc_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tcp.h"
#include "core_sdk/tty.h"
#include "core_sdk/wifi.h"

#define NC_APP_BUFFER_SIZE 256u
#define NC_APP_IO_TIMEOUT_MS 40u
#define NC_APP_EXIT_BYTE 0x1du
#define NC_APP_EXIT_BYTE_ALT 0x04u

static bool nc_app__parse_port(const char *text, uint16_t *out_port) {
    if (text == NULL || out_port == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT16_MAX) return false;
    *out_port = (uint16_t)value;
    return true;
}

static bruce_result_t nc_app__send_all(bruce_tcp_id_t socket, const char *data, size_t size) {
    size_t total = 0;
    while (total < size) {
        size_t sent = 0;
        bruce_result_t result = tcp__write(socket, data + total, size - total, 1000, &sent);
        if (result != BRUCE_OK) return result;
        if (sent == 0) return BRUCE_ERR_IO;
        total += sent;
    }
    return BRUCE_OK;
}

static bruce_result_t nc_app__forward_stdin(bruce_tcp_id_t socket, bool *out_exit) {
    char input[NC_APP_BUFFER_SIZE];
    size_t input_size = 0;
    bruce_result_t result = stdio__read(input, sizeof(input), 0, &input_size);
    if (result == BRUCE_ERR_TIMEOUT) return BRUCE_OK;
    if (result != BRUCE_OK) return result;

    char *exit_byte = memchr(input, NC_APP_EXIT_BYTE, input_size);
    char *exit_alt = memchr(input, NC_APP_EXIT_BYTE_ALT, input_size);
    if (exit_byte == NULL || (exit_alt != NULL && exit_alt < exit_byte)) exit_byte = exit_alt;
    size_t send_size = exit_byte != NULL ? (size_t)(exit_byte - input) : input_size;
    if (send_size > 0) {
        result = nc_app__send_all(socket, input, send_size);
        if (result != BRUCE_OK) return result;
        /* Local echo: a raw TCP relay has no line discipline, and the peer
         * may not echo what we send, so reflect sent bytes back so the user
         * can see what they just typed. Enter arrives as a bare '\r' (which
         * would only reset the cursor to column 0), so echo it as "\r\n" to
         * actually start a new line. */
        for (size_t i = 0; i < send_size; ++i) {
            if (input[i] == '\r') (void)stdio__write("\r\n", 2);
            else (void)stdio__write(&input[i], 1);
        }
    }
    *out_exit = exit_byte != NULL;
    return BRUCE_OK;
}

static bruce_result_t nc_app__session(bruce_tcp_id_t socket, bool *out_local_exit) {
    *out_local_exit = false;
    char received[NC_APP_BUFFER_SIZE + 1];
    for (;;) {
        size_t received_size = 0;
        bruce_result_t result =
            tcp__read(socket, received, NC_APP_BUFFER_SIZE, NC_APP_IO_TIMEOUT_MS, &received_size);
        if (result == BRUCE_OK) {
            if (received_size == 0) return BRUCE_OK;
            received[received_size] = '\0';
            (void)stdio__write(received, received_size);
        } else if (result != BRUCE_ERR_TIMEOUT) {
            return result;
        }

        bool exit_requested = false;
        result = nc_app__forward_stdin(socket, &exit_requested);
        if (result == BRUCE_ERR_NOT_FOUND || exit_requested) {
            *out_local_exit = true;
            return BRUCE_OK;
        }
        if (result != BRUCE_OK) return result;
        if (runtime__delay(1) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
}

static int nc_app__client(const char *host, uint16_t port) {
    if (!wifi__is_connected()) {
        stdio__printf("nc: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    stdio__printf("Connecting to %s:%u...\n", host, (unsigned int)port);
    bruce_tcp_id_t socket = BRUCE_TCP_ID_INVALID;
    bruce_result_t result = tcp__connect(host, port, 10000, &socket);
    if (result != BRUCE_OK) {
        stdio__printf("nc: connection failed (%d)\n", result);
        return result;
    }
    stdio__printf("Connected. Press Ctrl+] or Ctrl+D to close.\n");
    /* Raw byte relay: any byte, including Ctrl+C, must reach the remote peer
     * rather than get turned into a local SIGINT by the terminal. */
    (void)tty__set_mode(BRUCE_TTY_MODE_RAW);
    bool local_exit = false;
    result = nc_app__session(socket, &local_exit);
    (void)tty__set_mode(BRUCE_TTY_MODE_COOKED);
    (void)tcp__close(socket);
    stdio__printf("\nConnection closed%s\n", result == BRUCE_OK ? "." : " with an error.");
    return result;
}

static int nc_app__listener(uint16_t port) {
    if (!wifi__is_connected()) {
        stdio__printf("nc: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    bruce_tcp_id_t listener = BRUCE_TCP_ID_INVALID;
    bruce_result_t result = tcp__listen(port, &listener);
    if (result != BRUCE_OK) {
        stdio__printf("nc: could not listen on port %u (%d)\n", (unsigned int)port, result);
        return result;
    }

    const char *ip = wifi__get_ip();
    stdio__printf(
        "Listening on %s:%u. Press Ctrl+] or Ctrl+D to stop.\n", ip != NULL ? ip : "0.0.0.0", (unsigned int)port
    );
    for (;;) {
        bruce_tcp_id_t client = BRUCE_TCP_ID_INVALID;
        bruce_tcp_endpoint_t peer;
        result = tcp__accept(listener, 50, &client, &peer);
        if (result == BRUCE_ERR_TIMEOUT) {
            char input[NC_APP_BUFFER_SIZE];
            size_t input_size = 0;
            bruce_result_t input_result = stdio__read(input, sizeof(input), 0, &input_size);
            if (input_result == BRUCE_ERR_NOT_FOUND ||
                (input_result == BRUCE_OK &&
                 (memchr(input, NC_APP_EXIT_BYTE, input_size) != NULL ||
                  memchr(input, NC_APP_EXIT_BYTE_ALT, input_size) != NULL))) {
                result = BRUCE_OK;
                break;
            }
            if (runtime__delay(1) != BRUCE_OK) {
                result = BRUCE_ERR_CANCELLED;
                break;
            }
            continue;
        }
        if (result != BRUCE_OK) break;

        stdio__printf("Client connected from %s:%u\n", peer.host, (unsigned int)peer.port);
        (void)tty__set_mode(BRUCE_TTY_MODE_RAW);
        bool local_exit = false;
        bruce_result_t session_result = nc_app__session(client, &local_exit);
        (void)tty__set_mode(BRUCE_TTY_MODE_COOKED);
        (void)tcp__close(client);
        stdio__printf("\nClient disconnected%s\n", session_result == BRUCE_OK ? "." : " with an error.");
        if (local_exit || session_result == BRUCE_ERR_CANCELLED) {
            result = session_result;
            break;
        }
        stdio__printf("Waiting for another client. Press Ctrl+] or Ctrl+D to stop.\n");
    }

    (void)tcp__close(listener);
    stdio__printf("Listener stopped.\n");
    return result;
}

static int nc_app__gui(void) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Client",   .value = "client",  .icon_name = "cellphone"},
        {.label = "Server/Listener", .value = "listener", .icon_name = "server"},
    };
    size_t selected = 0;
    bruce_result_t result =     dialog__choice_launcher("nc", NULL, choices, 2, &selected);
    if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (result != BRUCE_OK) return result;
    bool client = strcmp(choices[selected].value, "listener") != 0;

    char port_text[8] = "";
    bruce_result_t port_result =
        dialog__number_input(client ? "nc client" : "nc listener", client ? "Remote port" : "Local port", NULL,
                             port_text, sizeof(port_text));
    if (port_result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (port_result != BRUCE_OK) return port_result;
    uint16_t port = 0;
    if (!nc_app__parse_port(port_text, &port)) {
        (void)dialog__message(BRUCE_DIALOG_ERROR, "nc", "Invalid port number");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char command[BRUCE_TCP_HOST_MAX + 64];
    if (client) {
        char host[BRUCE_TCP_HOST_MAX];
        bruce_result_t host_result =
            dialog__text_input("nc client", "Remote host", NULL, false, host, sizeof(host));
        if (host_result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
        if (host_result != BRUCE_OK) return host_result;
        if (host[0] == '\0') {
            (void)dialog__message(BRUCE_DIALOG_ERROR, "nc", "Host is required");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        snprintf(command, sizeof(command), "GUI=1 terminal nc %s %u", host, (unsigned int)port);
    } else {
        snprintf(command, sizeof(command), "GUI=1 terminal nc -l %u", (unsigned int)port);
    }
    return app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);
}

int nc_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(
        root,
        "netcat-style TCP client and listener. Connect with `nc <host> <port>` or listen with "
        "`nc -l <port>`. During a session, stdin is sent to the peer and received data is printed. "
        "Press Ctrl+] or Ctrl+D to close."
    );
    ap_set_version(root, "nc (BruceOS) 1.0");
    ap_add_flag(root, "l");
    ap_set_opt_help(root, "l", "Listen for an incoming connection instead of connecting out");
    ap_add_flag(root, "listen");
    ap_set_opt_help(root, "listen", "Alias for -l");
    ap_allow_extra_args(root);
    ap_first_pos_arg_ends_option_parsing(root);

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION) ap_print_help(root);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }

    if (!ap_has_args(root) && runtime__gui_requested()) {
        ap_free(root);
        return nc_app__gui();
    }

    bool listen = ap_found(root, "l") || ap_found(root, "listen");
    const char *first = ap_count_args(root) > 0 ? ap_get_arg_at_index(root, 0) : NULL;
    const char *second = ap_count_args(root) > 1 ? ap_get_arg_at_index(root, 1) : NULL;

    int result;
    if (listen) {
        uint16_t port = 0;
        if (!nc_app__parse_port(first, &port)) {
            stdio__printf("nc: invalid or missing port for listen mode (usage: nc -l <port>)\n");
            ap_print_help(root);
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            result = nc_app__listener(port);
        }
    } else if (first == NULL || second == NULL) {
        stdio__printf("nc: usage: nc <host> <port>  or  nc -l <port>\n");
        ap_print_help(root);
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else {
        uint16_t port = 0;
        if (!nc_app__parse_port(second, &port)) {
            stdio__printf("nc: invalid port\n");
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            char host[BRUCE_TCP_HOST_MAX];
            int length = snprintf(host, sizeof(host), "%s", first);
            result = length < 0 || (size_t)length >= sizeof(host) ? BRUCE_ERR_INVALID_ARGUMENT
                                                                  : nc_app__client(host, port);
        }
    }
    ap_free(root);
    return result;
}
