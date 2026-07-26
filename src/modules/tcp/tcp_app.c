#include "tcp_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/task.h"
#include "core_sdk/tcp.h"
#include "core_sdk/wifi.h"

#define TCP_APP_BUFFER_SIZE 256u
#define TCP_APP_IO_TIMEOUT_MS 20u
#define TCP_APP_EXIT_BYTE 0x1du

static void tcp_app__usage(void)
{
    printf("TCP terminal commands:\n");
    printf("  tcp client <host> <port>\n");
    printf("  tcp listener <port>\n");
    printf("During a session, stdin is sent to TCP and received data is printed to stdout.\n");
    printf("Press Ctrl+] to close.\n");
}

static bool tcp_app__parse_port(const char *text, uint16_t *out_port)
{
    if (text == NULL || out_port == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT16_MAX) return false;
    *out_port = (uint16_t)value;
    return true;
}

static bruce_result_t tcp_app__send_all(bruce_tcp_id_t socket, const char *data, size_t size)
{
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

static bruce_result_t tcp_app__forward_stdin(bruce_tcp_id_t socket, bool *out_exit)
{
    char input[TCP_APP_BUFFER_SIZE];
    size_t input_size = 0;
    bruce_result_t result = bruce_stdio_read(input, sizeof(input), 0, &input_size);
    if (result == BRUCE_ERR_TIMEOUT) return BRUCE_OK;
    if (result != BRUCE_OK) return result;

    char *exit_byte = memchr(input, TCP_APP_EXIT_BYTE, input_size);
    size_t send_size = exit_byte != NULL ? (size_t)(exit_byte - input) : input_size;
    if (send_size > 0) {
        result = tcp_app__send_all(socket, input, send_size);
        if (result != BRUCE_OK) return result;
    }
    *out_exit = exit_byte != NULL;
    return BRUCE_OK;
}

static bruce_result_t tcp_app__session(bruce_tcp_id_t socket, bool *out_local_exit)
{
    *out_local_exit = false;
    char received[TCP_APP_BUFFER_SIZE + 1];
    for (;;) {
        size_t received_size = 0;
        bruce_result_t result = tcp__read(socket, received, TCP_APP_BUFFER_SIZE,
                                          TCP_APP_IO_TIMEOUT_MS, &received_size);
        if (result == BRUCE_OK) {
            if (received_size == 0) return BRUCE_OK;
            received[received_size] = '\0';
            printf("%.*s", (int)received_size, received);
            fflush(stdout);
        } else if (result != BRUCE_ERR_TIMEOUT) {
            return result;
        }

        bool exit_requested = false;
        result = tcp_app__forward_stdin(socket, &exit_requested);
        if (result == BRUCE_ERR_NOT_FOUND || exit_requested) {
            *out_local_exit = true;
            return BRUCE_OK;
        }
        if (result != BRUCE_OK) return result;
        if (runtime__delay(1) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
}

static int tcp_app__client(const char *host, uint16_t port)
{
    if (!wifi__is_connected()) {
        printf("TCP client: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    printf("Connecting to %s:%u...\n", host, (unsigned int)port);
    bruce_tcp_id_t socket = BRUCE_TCP_ID_INVALID;
    bruce_result_t result = tcp__connect(host, port, 10000, &socket);
    if (result != BRUCE_OK) {
        printf("TCP client: connection failed (%d)\n", result);
        return result;
    }
    printf("Connected. Press Ctrl+] to close.\n");
    bool local_exit = false;
    result = tcp_app__session(socket, &local_exit);
    (void)tcp__close(socket);
    printf("\nConnection closed%s\n", result == BRUCE_OK ? "." : " with an error.");
    return result;
}

static int tcp_app__listener(uint16_t port)
{
    if (!wifi__is_connected()) {
        printf("TCP listener: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    bruce_tcp_id_t listener = BRUCE_TCP_ID_INVALID;
    bruce_result_t result = tcp__listen(port, &listener);
    if (result != BRUCE_OK) {
        printf("TCP listener: could not listen on port %u (%d)\n", (unsigned int)port, result);
        return result;
    }

    const char *ip = wifi__get_ip();
    printf("Listening on %s:%u. Press Ctrl+] to stop.\n", ip != NULL ? ip : "0.0.0.0",
           (unsigned int)port);
    for (;;) {
        bruce_tcp_id_t client = BRUCE_TCP_ID_INVALID;
        bruce_tcp_endpoint_t peer;
        result = tcp__accept(listener, 50, &client, &peer);
        if (result == BRUCE_ERR_TIMEOUT) {
            char input[TCP_APP_BUFFER_SIZE];
            size_t input_size = 0;
            bruce_result_t input_result = bruce_stdio_read(input, sizeof(input), 0, &input_size);
            if (input_result == BRUCE_ERR_NOT_FOUND ||
                (input_result == BRUCE_OK && memchr(input, TCP_APP_EXIT_BYTE, input_size) != NULL)) {
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

        printf("Client connected from %s:%u\n", peer.host, (unsigned int)peer.port);
        bool local_exit = false;
        bruce_result_t session_result = tcp_app__session(client, &local_exit);
        (void)tcp__close(client);
        printf("\nClient disconnected%s\n", session_result == BRUCE_OK ? "." : " with an error.");
        if (local_exit || session_result == BRUCE_ERR_CANCELLED) {
            result = session_result;
            break;
        }
        printf("Waiting for another client. Press Ctrl+] to stop.\n");
    }

    (void)tcp__close(listener);
    printf("Listener stopped.\n");
    return result;
}

int tcp_app_main(int argc, char **argv)
{
    if (argc == 0 || argv == NULL || argv[0] == NULL || strcmp(argv[0], "help") == 0) {
        tcp_app__usage();
        return 0;
    }

    uint16_t port = 0;
    if ((strcmp(argv[0], "client") == 0 || strcmp(argv[0], "connect") == 0) && argc == 3 &&
        tcp_app__parse_port(argv[2], &port)) {
        return tcp_app__client(argv[1], port);
    }
    if ((strcmp(argv[0], "listener") == 0 || strcmp(argv[0], "listen") == 0) && argc == 2 &&
        tcp_app__parse_port(argv[1], &port)) {
        return tcp_app__listener(port);
    }

    tcp_app__usage();
    return BRUCE_ERR_INVALID_ARGUMENT;
}
