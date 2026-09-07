#include "nc_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tcp.h"
#include "core_sdk/tty.h"
#include "core_sdk/udp.h"
#include "core_sdk/wifi.h"

#define NC_APP_BUFFER_SIZE 256u
#define NC_APP_IO_TIMEOUT_MS 40u
#define NC_APP_EXIT_BYTE 0x1du
#define NC_APP_EXIT_BYTE_ALT 0x04u
#define NC_APP_DEFAULT_CONNECT_TIMEOUT_MS 10000u
#define NC_APP_DEFAULT_SCAN_TIMEOUT_MS 500u
#define NC_APP_UDP_SEND_TIMEOUT_MS 1000u

static bool nc_app__parse_port(const char *text, uint16_t *out_port) {
    if (text == NULL || out_port == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT16_MAX) return false;
    *out_port = (uint16_t)value;
    return true;
}

/* Accepts either a single port ("80") or a "low-high" range ("20-25") for -z
 * scan mode. */
static bool nc_app__parse_port_range(const char *text, uint16_t *out_low, uint16_t *out_high) {
    if (text == NULL || out_low == NULL || out_high == NULL) return false;
    const char *dash = strchr(text, '-');
    if (dash == NULL) {
        uint16_t port;
        if (!nc_app__parse_port(text, &port)) return false;
        *out_low = port;
        *out_high = port;
        return true;
    }
    size_t low_len = (size_t)(dash - text);
    char low_text[8];
    if (low_len == 0 || low_len >= sizeof(low_text)) return false;
    memcpy(low_text, text, low_len);
    low_text[low_len] = '\0';
    uint16_t low, high;
    if (!nc_app__parse_port(low_text, &low)) return false;
    if (!nc_app__parse_port(dash + 1, &high)) return false;
    if (low > high) return false;
    *out_low = low;
    *out_high = high;
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

/* Every per-invocation flag that isn't specific to one mode (host/port
 * aside), bundled so the mode functions below take one pointer instead of an
 * ever-growing parameter list. */
typedef struct {
    bool verbose;              /* -v */
    bool crlf;                 /* -C */
    bool keep_listening;       /* -k, TCP listener only */
    uint16_t local_port;       /* -p, 0 = OS-assigned */
    uint32_t timeout_ms;       /* -w, resolved to the caller's mode-specific default already */
    uint32_t interval_ms;      /* -i, 0 = no pacing delay */
    uint32_t quit_after_ms;    /* -q, UINT32_MAX = never auto-quit on local stdin EOF (real nc's default) */
    const char *exec_cmd;      /* -e, NULL = interactive */
    bruce_file_id_t dump_file; /* -o, BRUCE_FILE_ID_INVALID = not dumping */
} nc_app__options_t;

/* A tiny TCP/UDP union so the interactive relay (nc_app__session()) and the
 * -e exec relay (nc_app__exec_session()) only need to be written once each,
 * instead of once per protocol. UDP has no real "connection" -- udp_peer is
 * the peer bytes are currently addressed to: pre-set to the target for a
 * client, and updated to whoever last sent a datagram for a listener (same
 * "reply to whoever spoke most recently" convention an interactive `nc -u
 * -l` session needs, since a UDP socket has no fixed peer of its own). */
typedef struct {
    bool is_udp;
    bruce_tcp_id_t tcp_socket;
    bruce_udp_id_t udp_socket;
    bruce_udp_endpoint_t udp_peer;
    bool udp_peer_known;
    bruce_file_id_t dump_file;      /* -o, BRUCE_FILE_ID_INVALID when not dumping */
    uint64_t dump_sent_offset;      /* running per-direction offset columns, this session only */
    uint64_t dump_received_offset;
} nc_app__transport_t;

/* -o: formats `size` bytes as classic hexdump -C style lines ("OFFSET  hex
 * hex ... hex  |ascii|"), each prefixed with `direction` ('>' sent to the
 * peer, '<' received from it) so one file captures both sides of the
 * exchange distinguishably -- real nc's -o doesn't tag direction at all,
 * this is a small superset since our relay already sees both directions
 * through one place anyway. Advances *offset by `size`; failures are not
 * fatal to the session, just silently drop that chunk of the log. */
static void nc_app__dump_chunk(bruce_file_id_t file, char direction, const void *data, size_t size, uint64_t *offset) {
    const uint8_t *bytes = (const uint8_t *)data;
    char line[96];
    for (size_t base = 0; base < size; base += 16) {
        size_t chunk = size - base < 16 ? size - base : 16;
        int pos = snprintf(line, sizeof(line), "%c %08llx  ", direction, (unsigned long long)(*offset + base));
        for (size_t i = 0; i < 16 && pos > 0 && (size_t)pos < sizeof(line); ++i) {
            pos += snprintf(
                line + pos, sizeof(line) - (size_t)pos, i < chunk ? "%02x " : "   ", bytes[base + i]
            );
            if (i == 7) pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " ");
        }
        if (pos < 0 || (size_t)pos + chunk + 3 > sizeof(line)) continue;
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " |");
        for (size_t i = 0; i < chunk; ++i) {
            char c = (char)bytes[base + i];
            line[pos++] = (c >= 0x20 && c < 0x7f) ? c : '.';
        }
        line[pos++] = '|';
        line[pos++] = '\n';
        size_t written = 0;
        (void)storage__write(file, line, (size_t)pos, &written);
    }
    *offset += size;
}

static bruce_result_t
nc_app__transport_read(nc_app__transport_t *transport, void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size) {
    bruce_result_t result;
    if (transport->is_udp) {
        bruce_udp_endpoint_t peer;
        result = udp__receive_from(transport->udp_socket, buffer, capacity, timeout_ms, out_size, &peer);
        if (result == BRUCE_OK && *out_size > 0) {
            transport->udp_peer = peer;
            transport->udp_peer_known = true;
        }
    } else {
        result = tcp__read(transport->tcp_socket, buffer, capacity, timeout_ms, out_size);
    }
    if (result == BRUCE_OK && *out_size > 0 && transport->dump_file != BRUCE_FILE_ID_INVALID) {
        nc_app__dump_chunk(transport->dump_file, '<', buffer, *out_size, &transport->dump_received_offset);
    }
    return result;
}

static bruce_result_t nc_app__transport_write(nc_app__transport_t *transport, const void *buffer, size_t size) {
    bruce_result_t result;
    if (transport->is_udp) {
        /* Nothing to reply to yet (a fresh listener with no datagram received
         * so far) -- silently drop rather than fail the whole session. */
        if (!transport->udp_peer_known) return BRUCE_OK;
        size_t sent = 0;
        result = udp__send_to(
            transport->udp_socket, transport->udp_peer.host, transport->udp_peer.port, buffer, size,
            NC_APP_UDP_SEND_TIMEOUT_MS, &sent
        );
    } else {
        result = nc_app__send_all(transport->tcp_socket, buffer, size);
    }
    if (result == BRUCE_OK && size > 0 && transport->dump_file != BRUCE_FILE_ID_INVALID) {
        nc_app__dump_chunk(transport->dump_file, '>', buffer, size, &transport->dump_sent_offset);
    }
    return result;
}

static bruce_result_t
nc_app__forward_stdin(nc_app__transport_t *transport, const nc_app__options_t *opts, bool *out_exit) {
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
        bruce_result_t send_result;
        if (opts->crlf) {
            /* -C: send bare '\r'/'\n' as "\r\n" -- needed to manually talk to
             * line-oriented text protocols (SMTP/FTP/HTTP-1.0/POP3), which
             * expect CRLF; a raw terminal doesn't produce that on its own. */
            char translated[NC_APP_BUFFER_SIZE * 2];
            size_t translated_size = 0;
            for (size_t i = 0; i < send_size && translated_size + 2 <= sizeof(translated); ++i) {
                if (input[i] == '\r' || input[i] == '\n') {
                    translated[translated_size++] = '\r';
                    translated[translated_size++] = '\n';
                    if (input[i] == '\r' && i + 1 < send_size && input[i + 1] == '\n') ++i; /* don't double a real "\r\n" */
                } else {
                    translated[translated_size++] = input[i];
                }
            }
            send_result = nc_app__transport_write(transport, translated, translated_size);
        } else {
            send_result = nc_app__transport_write(transport, input, send_size);
        }
        if (send_result != BRUCE_OK) return send_result;
        /* Local echo: a raw relay has no line discipline, and the peer may
         * not echo what we send, so reflect sent bytes back so the user can
         * see what they just typed. Enter arrives as a bare '\r' (which
         * would only reset the cursor to column 0), so echo it as "\r\n" to
         * actually start a new line. */
        for (size_t i = 0; i < send_size; ++i) {
            if (input[i] == '\r') (void)stdio__write("\r\n", 2);
            else (void)stdio__write(&input[i], 1);
        }
        /* -i: pace sent lines, e.g. to avoid flooding a fragile peer. */
        if (opts->interval_ms > 0 && runtime__delay(opts->interval_ms) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    *out_exit = exit_byte != NULL;
    return BRUCE_OK;
}

static bruce_result_t
nc_app__session(nc_app__transport_t *transport, const nc_app__options_t *opts, bool *out_local_exit) {
    *out_local_exit = false;
    char received[NC_APP_BUFFER_SIZE + 1];
    bool stdin_eof = false;
    uint32_t eof_linger_elapsed_ms = 0;
    for (;;) {
        size_t received_size = 0;
        bruce_result_t result =
            nc_app__transport_read(transport, received, NC_APP_BUFFER_SIZE, NC_APP_IO_TIMEOUT_MS, &received_size);
        if (result == BRUCE_OK) {
            if (received_size > 0) {
                received[received_size] = '\0';
                (void)stdio__write(received, received_size);
            } else if (!transport->is_udp) {
                /* TCP EOF from the peer -- session is over regardless of -q;
                 * UDP has no equivalent (a zero-size datagram, if it ever
                 * happens, isn't a "connection closed" signal). */
                return BRUCE_OK;
            }
        } else if (result != BRUCE_ERR_TIMEOUT) {
            return result;
        }

        if (!stdin_eof) {
            bool exit_requested = false;
            bruce_result_t forward_result = nc_app__forward_stdin(transport, opts, &exit_requested);
            if (exit_requested) {
                *out_local_exit = true;
                return BRUCE_OK;
            }
            if (forward_result == BRUCE_ERR_NOT_FOUND) {
                /* Local stdin closed (e.g. redirected input exhausted). Real
                 * nc's default is to keep draining the peer until it closes
                 * the connection or the operator cancels -- -q overrides
                 * that with a linger timer instead of waiting forever. */
                stdin_eof = true;
            } else if (forward_result != BRUCE_OK) {
                return forward_result;
            }
        } else if (opts->quit_after_ms != UINT32_MAX) {
            eof_linger_elapsed_ms += NC_APP_IO_TIMEOUT_MS;
            if (eof_linger_elapsed_ms >= opts->quit_after_ms) {
                *out_local_exit = true;
                return BRUCE_OK;
            }
        }
        if (runtime__delay(1) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
}

/* -e/-c "exec on connect": runs `exec_cmd` ("<app_name> [arg text]", the same
 * shape app_runner__run() itself takes) as a child process with its stdio
 * wired to the peer instead of the terminal -- a real bind/reverse shell
 * primitive, not a demo of one. Built on the session-routing API
 * (core_sdk/stdio.h) rather than any fork/exec/dup2 (BruceOS has neither):
 * the child's stdio is captured into a session this loop owns, so pumping
 * bytes between that session and the transport *is* wiring the child to the
 * peer. Unlike the interactive nc_app__session(), local stdin is never
 * touched here -- once -e hands off, the operator is not part of the byte
 * stream (matches real nc -e). */
static bruce_result_t nc_app__exec_session(nc_app__transport_t *transport, const nc_app__options_t *opts) {
    const char *exec_cmd = opts->exec_cmd;
    bool verbose = opts->verbose;
    char app_name[BRUCE_PROCESS_NAME_MAX];
    const char *space = strchr(exec_cmd, ' ');
    size_t name_len = space != NULL ? (size_t)(space - exec_cmd) : strlen(exec_cmd);
    if (name_len >= sizeof(app_name)) name_len = sizeof(app_name) - 1;
    memcpy(app_name, exec_cmd, name_len);
    app_name[name_len] = '\0';
    const char *rest = space != NULL ? space + 1 : NULL;

    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    bruce_result_t result = stdio__session_create(&session);
    if (result != BRUCE_OK) return result;

    bruce_result_t route_result = stdio__session_route_children(session);
    if (route_result != BRUCE_OK) {
        (void)stdio__session_close(session);
        return route_result;
    }
    int launched = app_runner__run(app_name, rest, BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID); /* restore default routing */
    if (launched <= 0) {
        (void)stdio__session_close(session);
        stdio__printf("nc: -e: could not start '%s'\n", app_name);
        return BRUCE_ERR_IO;
    }
    bruce_process_id_t child = (bruce_process_id_t)launched;
    if (verbose) stdio__printf("nc: -e: running '%s', its stdio is now the peer's\n", app_name);

    bruce_result_t loop_result = BRUCE_OK;
    bool child_exited = false;
    for (;;) {
        char buffer[NC_APP_BUFFER_SIZE];
        size_t received_size = 0;
        bruce_result_t read_result =
            nc_app__transport_read(transport, buffer, sizeof(buffer), NC_APP_IO_TIMEOUT_MS, &received_size);
        if (read_result == BRUCE_OK && received_size > 0) {
            (void)stdio__session_write_input(session, buffer, received_size);
        } else if (read_result == BRUCE_OK && received_size == 0 && !transport->is_udp) {
            (void)stdio__session_close_input(session); /* peer closed -- signal EOF to the child */
        } else if (read_result != BRUCE_ERR_TIMEOUT) {
            loop_result = read_result;
            break;
        }

        size_t output_size = 0;
        bruce_result_t output_result = stdio__session_read_output(session, buffer, sizeof(buffer), &output_size);
        if (output_result == BRUCE_OK && output_size > 0) {
            bruce_result_t write_result = nc_app__transport_write(transport, buffer, output_size);
            if (write_result != BRUCE_OK) {
                loop_result = write_result;
                break;
            }
            continue; /* keep draining output before waiting/polling again */
        }

        if (!child_exited && process__wait(child, 0) == BRUCE_OK) child_exited = true;
        if (child_exited) break; /* last output already drained by the check above */

        if (runtime__delay(1) != BRUCE_OK) {
            loop_result = BRUCE_ERR_CANCELLED;
            break;
        }
    }

    if (!child_exited) {
        (void)stdio__session_close_input(session);
        (void)process__terminate(child); /* don't leave an orphaned child behind on error/cancel */
    }
    bruce_process_status_t status = {0};
    (void)process__wait_status(child, 2000, &status);
    (void)stdio__session_close(session);
    return loop_result;
}

static int nc_app__client(const char *host, uint16_t port, const nc_app__options_t *opts) {
    if (!wifi__is_connected()) {
        stdio__printf("nc: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    stdio__printf("Connecting to %s:%u...\n", host, (unsigned int)port);
    bruce_tcp_id_t socket = BRUCE_TCP_ID_INVALID;
    bruce_result_t result = tcp__connect_from(host, port, opts->local_port, opts->timeout_ms, &socket);
    if (result != BRUCE_OK) {
        stdio__printf("nc: connection failed (%d)\n", result);
        return result;
    }

    nc_app__transport_t transport = {.is_udp = false, .tcp_socket = socket, .dump_file = opts->dump_file};
    if (opts->exec_cmd != NULL) {
        if (opts->verbose) stdio__printf("nc: connected (tcp), handing off to -e\n");
        result = nc_app__exec_session(&transport, opts);
        (void)tcp__close(socket);
        stdio__printf("\n-e session ended%s\n", result == BRUCE_OK ? "." : " with an error.");
        return result;
    }

    stdio__printf("Connected. Press Ctrl+] or Ctrl+D to close.\n");
    /* Raw byte relay: any byte, including Ctrl+C, must reach the remote peer
     * rather than get turned into a local SIGINT by the terminal. */
    (void)tty__set_mode(BRUCE_TTY_MODE_RAW);
    bool local_exit = false;
    result = nc_app__session(&transport, opts, &local_exit);
    (void)tty__set_mode(BRUCE_TTY_MODE_COOKED);
    (void)tcp__close(socket);
    stdio__printf("\nConnection closed%s\n", result == BRUCE_OK ? "." : " with an error.");
    return result;
}

static int nc_app__listener(uint16_t port, const nc_app__options_t *opts) {
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
        nc_app__transport_t transport = {.is_udp = false, .tcp_socket = client, .dump_file = opts->dump_file};
        bool local_exit = false;
        bruce_result_t session_result;
        if (opts->exec_cmd != NULL) {
            session_result = nc_app__exec_session(&transport, opts);
        } else {
            (void)tty__set_mode(BRUCE_TTY_MODE_RAW);
            session_result = nc_app__session(&transport, opts, &local_exit);
            (void)tty__set_mode(BRUCE_TTY_MODE_COOKED);
        }
        (void)tcp__close(client);
        stdio__printf("\nClient disconnected%s\n", session_result == BRUCE_OK ? "." : " with an error.");
        /* Real nc's default is to handle exactly one connection and exit;
         * -k is what makes it loop for more. An explicit local exit (Ctrl+]
         * or Ctrl+D) or a cancellation always stops the listener regardless
         * of -k. */
        if (local_exit || session_result == BRUCE_ERR_CANCELLED || !opts->keep_listening) {
            result = session_result;
            break;
        }
        stdio__printf("Waiting for another client. Press Ctrl+] or Ctrl+D to stop.\n");
    }

    (void)tcp__close(listener);
    stdio__printf("Listener stopped.\n");
    return result;
}

static int nc_app__udp_client(const char *host, uint16_t port, const nc_app__options_t *opts) {
    if (!wifi__is_connected()) {
        stdio__printf("nc: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    bruce_udp_id_t socket = BRUCE_UDP_ID_INVALID;
    bruce_result_t result = udp__open(opts->local_port, &socket);
    if (result != BRUCE_OK) {
        stdio__printf("nc: could not open a UDP socket (%d)\n", result);
        return result;
    }
    nc_app__transport_t transport = {
        .is_udp = true, .udp_socket = socket, .udp_peer_known = true, .dump_file = opts->dump_file
    };
    snprintf(transport.udp_peer.host, sizeof(transport.udp_peer.host), "%s", host);
    transport.udp_peer.port = port;

    if (opts->exec_cmd != NULL) {
        if (opts->verbose) stdio__printf("nc: udp target %s:%u, handing off to -e\n", host, (unsigned int)port);
        result = nc_app__exec_session(&transport, opts);
        stdio__printf("\n-e session ended%s\n", result == BRUCE_OK ? "." : " with an error.");
    } else {
        stdio__printf("Sending to %s:%u (udp). Press Ctrl+] or Ctrl+D to close.\n", host, (unsigned int)port);
        (void)tty__set_mode(BRUCE_TTY_MODE_RAW);
        bool local_exit = false;
        result = nc_app__session(&transport, opts, &local_exit);
        (void)tty__set_mode(BRUCE_TTY_MODE_COOKED);
        stdio__printf("\nSession closed%s\n", result == BRUCE_OK ? "." : " with an error.");
    }
    (void)udp__close(socket);
    return result;
}

static int nc_app__udp_listener(uint16_t port, const nc_app__options_t *opts) {
    if (!wifi__is_connected()) {
        stdio__printf("nc: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    bruce_udp_id_t socket = BRUCE_UDP_ID_INVALID;
    bruce_result_t result = udp__open(port, &socket);
    if (result != BRUCE_OK) {
        stdio__printf("nc: could not bind UDP port %u (%d)\n", (unsigned int)port, result);
        return result;
    }
    stdio__printf("Listening on UDP port %u. Press Ctrl+] or Ctrl+D to stop.\n", (unsigned int)port);
    /* No accept loop -- a UDP socket has no separate listener/connection
     * split, so unlike the TCP listener this is a single session for the
     * whole run (and -k is a no-op here, same as real nc); nc_app__transport
     * _write()'s "reply to whoever sent last" rule effectively re-targets it
     * as different peers send datagrams. */
    nc_app__transport_t transport = {
        .is_udp = true, .udp_socket = socket, .udp_peer_known = false, .dump_file = opts->dump_file
    };
    if (opts->exec_cmd != NULL) {
        result = nc_app__exec_session(&transport, opts);
        stdio__printf("\n-e session ended%s\n", result == BRUCE_OK ? "." : " with an error.");
    } else {
        (void)tty__set_mode(BRUCE_TTY_MODE_RAW);
        bool local_exit = false;
        result = nc_app__session(&transport, opts, &local_exit);
        (void)tty__set_mode(BRUCE_TTY_MODE_COOKED);
        stdio__printf("\nSession closed%s\n", result == BRUCE_OK ? "." : " with an error.");
    }
    (void)udp__close(socket);
    return result;
}

/* -z zero-I/O mode: a TCP connect-scan. Each port's "openness" is exactly
 * whether tcp__connect() succeeds within timeout_ms -- no data is ever sent.
 * UDP scanning is deliberately not supported (see the -z/-u conflict check
 * in nc_app_main()): a UDP "connect" always succeeds locally, real openness
 * can only be inferred from either an application-level reply or an ICMP
 * port-unreachable, and this SDK exposes neither raw ICMP nor a generic
 * "did anything answer" probe. */
static int nc_app__scan(const char *host, uint16_t low, uint16_t high, const nc_app__options_t *opts) {
    if (!wifi__is_connected()) {
        stdio__printf("nc: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    stdio__printf("Scanning %s %u-%u (tcp)...\n", host, (unsigned int)low, (unsigned int)high);
    unsigned int open_count = 0;
    for (uint32_t port = low; port <= high; ++port) {
        bruce_tcp_id_t socket = BRUCE_TCP_ID_INVALID;
        bruce_result_t result = tcp__connect(host, (uint16_t)port, opts->timeout_ms, &socket);
        if (result == BRUCE_OK) {
            stdio__printf("%u/tcp open\n", (unsigned int)port);
            ++open_count;
            (void)tcp__close(socket);
        } else if (opts->verbose) {
            stdio__printf("%u/tcp closed\n", (unsigned int)port);
        }

        /* Let a Ctrl+]/Ctrl+D (or an externally delivered cooperative signal)
         * abort a long range scan instead of it running to completion. */
        char input[1];
        size_t input_size = 0;
        bruce_result_t poll_result = stdio__read(input, sizeof(input), 0, &input_size);
        if (poll_result == BRUCE_ERR_NOT_FOUND ||
            (input_size > 0 && (input[0] == (char)NC_APP_EXIT_BYTE || input[0] == (char)NC_APP_EXIT_BYTE_ALT))) {
            stdio__printf("Scan cancelled.\n");
            return BRUCE_OK;
        }
        /* -i: pace between ports, e.g. to be a less obtrusive scan. */
        if (runtime__delay(opts->interval_ms > 0 ? opts->interval_ms : 1) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    stdio__printf("Scan complete: %u open port(s) found.\n", open_count);
    return BRUCE_OK;
}

static int nc_app__gui(void) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Client",          .value = "client",   .icon_name = "cellphone"},
        {.label = "Server/Listener", .value = "listener", .icon_name = "server"},
        {.label = "Port scan",       .value = "scan",      .icon_name = "radar"},
        {.label = "Back",            .value = "back"},
    };
    size_t selected = 0;
    bruce_result_t result = dialog__choice_launcher("nc", NULL, choices, 4, &selected);
    if (result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (result != BRUCE_OK) return result;
    if (strcmp(choices[selected].value, "back") == 0) return BRUCE_OK;
    bool scan = strcmp(choices[selected].value, "scan") == 0;
    bool client = !scan && strcmp(choices[selected].value, "listener") != 0;

    char command[BRUCE_TCP_HOST_MAX + 64];
    if (scan) {
        char host[BRUCE_TCP_HOST_MAX];
        bruce_result_t host_result = dialog__text_input("nc scan", "Target host", NULL, false, host, sizeof(host));
        if (host_result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
        if (host_result != BRUCE_OK) return host_result;
        if (host[0] == '\0') {
            (void)dialog__message(BRUCE_DIALOG_ERROR, "nc", "Host is required");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        char range[16] = "";
        bruce_result_t range_result =
            dialog__text_input("nc scan", "Port or low-high range", "1-1024", false, range, sizeof(range));
        if (range_result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
        if (range_result != BRUCE_OK) return range_result;
        if (range[0] == '\0') snprintf(range, sizeof(range), "1-1024");
        snprintf(command, sizeof(command), "GUI=1 terminal nc -z -v %s %s", host, range);
        return app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);
    }

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
        "netcat-style TCP/UDP client, listener, and port scanner. Connect with `nc <host> <port>`, "
        "listen with `nc -l <port>` (handles one connection then exits, unless -k), scan with "
        "`nc -z <host> <port|low-high>`. During an interactive session, stdin is sent to the peer and "
        "received data is printed -- press Ctrl+] or Ctrl+D to close. Without -q, closing local stdin "
        "does not end the session: nc keeps draining the peer until it closes the connection or you "
        "press Ctrl+]/Ctrl+D."
    );
    ap_set_version(root, "nc (BruceOS) 1.0");
    ap_add_flag(root, "l");
    ap_set_opt_help(root, "l", "Listen for an incoming connection instead of connecting out");
    ap_add_flag(root, "listen");
    ap_set_opt_help(root, "listen", "Alias for -l");
    ap_add_flag(root, "u");
    ap_set_opt_help(root, "u", "Use UDP instead of TCP");
    ap_add_flag(root, "udp");
    ap_set_opt_help(root, "udp", "Alias for -u");
    ap_add_flag(root, "z");
    ap_set_opt_help(
        root, "z",
        "Zero-I/O mode: scan for open TCP ports instead of connecting; the port argument may be a "
        "single port or a low-high range"
    );
    ap_add_flag(root, "v");
    ap_set_opt_help(root, "v", "Verbose: also report closed ports while scanning, and extra connection detail");
    ap_add_flag(root, "verbose");
    ap_set_opt_help(root, "verbose", "Alias for -v");
    ap_add_int_opt(root, "w", 0);
    ap_set_opt_help(
        root, "w", "Timeout in seconds (default: 10s to connect, 0.5s per port while scanning)"
    );
    ap_add_str_opt(root, "e", "");
    ap_set_opt_help(
        root, "e",
        "Run '<cmd> [arg text]' with its stdio wired to the peer once connected, instead of the "
        "terminal -- a bind/reverse shell primitive. <cmd> is an app_runner command name (e.g. "
        "'shell -i'). Not usable with -z."
    );
    ap_add_int_opt(root, "p", 0);
    ap_set_opt_help(
        root, "p",
        "Source port for an outgoing connection, instead of an OS-assigned one -- e.g. to reach a "
        "peer whose firewall trusts a specific source port. Client/-e modes only."
    );
    ap_add_str_opt(root, "s", "");
    ap_set_opt_help(
        root, "s",
        "Source address (accepted, not applied -- BruceOS is single-homed, there is no second local "
        "address to bind; kept for command-line compatibility with real nc)"
    );
    ap_add_flag(root, "k");
    ap_set_opt_help(root, "k", "Keep listening for further connections after one closes (listen mode only)");
    ap_add_flag(root, "C");
    ap_set_opt_help(root, "C", "Translate outgoing bare CR/LF to CRLF -- useful when talking a line-oriented "
                               "text protocol (SMTP, HTTP/1.0, ...) by hand");
    ap_add_int_opt(root, "i", 0);
    ap_set_opt_help(
        root, "i", "Delay in seconds between sent lines (interactive/-e modes) or between scanned ports (-z)"
    );
    ap_add_int_opt(root, "q", -1);
    ap_set_opt_help(
        root, "q",
        "Seconds to keep reading the peer after local stdin closes, then quit (default: never auto-quit, "
        "matching real nc -- keep draining the peer until it closes or you cancel). 0 quits immediately "
        "on stdin EOF."
    );
    ap_add_str_opt(root, "o", "");
    ap_set_opt_help(root, "o", "Hex-dump the full session (both directions) to <file>");
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
    bool udp = ap_found(root, "u") || ap_found(root, "udp");
    bool scan = ap_found(root, "z");
    nc_app__options_t opts = {0};
    opts.verbose = ap_found(root, "v") || ap_found(root, "verbose");
    opts.crlf = ap_found(root, "C");
    opts.keep_listening = ap_found(root, "k");
    opts.dump_file = BRUCE_FILE_ID_INVALID;
    int timeout_seconds = ap_found(root, "w") ? ap_get_int_value(root, "w") : 0;
    opts.exec_cmd = ap_found(root, "e") ? ap_get_str_value(root, "e") : NULL;
    if (opts.exec_cmd != NULL && opts.exec_cmd[0] == '\0') opts.exec_cmd = NULL;
    int local_port_value = ap_found(root, "p") ? ap_get_int_value(root, "p") : 0;
    const char *source_address = ap_found(root, "s") ? ap_get_str_value(root, "s") : NULL;
    if (source_address != NULL && source_address[0] == '\0') source_address = NULL;
    if (source_address != NULL) {
        stdio__printf(
            "nc: -s is accepted but not applied -- BruceOS is single-homed, there is no second local "
            "address to bind\n"
        );
    }
    int interval_seconds = ap_found(root, "i") ? ap_get_int_value(root, "i") : 0;
    bool quit_given = ap_found(root, "q");
    int quit_seconds = quit_given ? ap_get_int_value(root, "q") : -1;
    const char *dump_path = ap_found(root, "o") ? ap_get_str_value(root, "o") : NULL;
    if (dump_path != NULL && dump_path[0] == '\0') dump_path = NULL;

    const char *first = ap_count_args(root) > 0 ? ap_get_arg_at_index(root, 0) : NULL;
    const char *second = ap_count_args(root) > 1 ? ap_get_arg_at_index(root, 1) : NULL;

    int result;
    if (timeout_seconds < 0) {
        stdio__printf("nc: -w must be a positive number of seconds\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (local_port_value < 0 || local_port_value > UINT16_MAX) {
        stdio__printf("nc: -p must be a valid port number (1-65535)\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (local_port_value != 0 && listen) {
        stdio__printf("nc: -p is not meaningful with -l -- the listen port is already the fixed local port\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (local_port_value != 0 && scan) {
        stdio__printf("nc: -p is not supported with -z\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (interval_seconds < 0) {
        stdio__printf("nc: -i must be a positive number of seconds\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (quit_given && quit_seconds < 0) {
        stdio__printf("nc: -q must be a non-negative number of seconds\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (scan && listen) {
        stdio__printf("nc: -z (scan) and -l (listen) cannot be combined\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (scan && udp) {
        stdio__printf(
            "nc: -z (scan) does not support -u -- UDP has no handshake to confirm a port is open, "
            "and this build has no raw-ICMP access to notice a port-unreachable reply either\n"
        );
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (scan && opts.exec_cmd != NULL) {
        stdio__printf("nc: -z (scan) and -e cannot be combined\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (scan && quit_given) {
        stdio__printf("nc: -q is not meaningful with -z -- a scan has no peer session to linger on\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (scan && dump_path != NULL) {
        stdio__printf("nc: -o is not meaningful with -z -- a scan sends no session data to dump\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (!listen && opts.keep_listening) {
        stdio__printf("nc: -k is only meaningful with -l\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else {
        opts.local_port = (uint16_t)local_port_value;
        opts.interval_ms = (uint32_t)interval_seconds * 1000u;
        opts.quit_after_ms = quit_given ? (uint32_t)quit_seconds * 1000u : UINT32_MAX;

        if (dump_path != NULL) {
            bruce_result_t open_result = storage__open(
                dump_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
                &opts.dump_file
            );
            if (open_result != BRUCE_OK) {
                stdio__printf("nc: -o: could not open '%s' for writing (%d)\n", dump_path, open_result);
                ap_free(root);
                return open_result;
            }
        }

        if (listen) {
            uint16_t port = 0;
            if (!nc_app__parse_port(first, &port)) {
                stdio__printf("nc: invalid or missing port for listen mode (usage: nc -l <port>)\n");
                ap_print_help(root);
                result = BRUCE_ERR_INVALID_ARGUMENT;
            } else {
                result = udp ? nc_app__udp_listener(port, &opts) : nc_app__listener(port, &opts);
            }
        } else if (scan) {
            uint16_t low = 0, high = 0;
            if (first == NULL || second == NULL) {
                stdio__printf("nc: usage: nc -z <host> <port|low-high>\n");
                ap_print_help(root);
                result = BRUCE_ERR_INVALID_ARGUMENT;
            } else if (!nc_app__parse_port_range(second, &low, &high)) {
                stdio__printf("nc: invalid port or port range\n");
                result = BRUCE_ERR_INVALID_ARGUMENT;
            } else {
                char host[BRUCE_TCP_HOST_MAX];
                int length = snprintf(host, sizeof(host), "%s", first);
                opts.timeout_ms =
                    timeout_seconds > 0 ? (uint32_t)timeout_seconds * 1000u : NC_APP_DEFAULT_SCAN_TIMEOUT_MS;
                result = length < 0 || (size_t)length >= sizeof(host) ? BRUCE_ERR_INVALID_ARGUMENT
                                                                       : nc_app__scan(host, low, high, &opts);
            }
        } else if (first == NULL || second == NULL) {
            stdio__printf("nc: usage: nc <host> <port>  or  nc -l <port>  or  nc -z <host> <port|low-high>\n");
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
                if (length < 0 || (size_t)length >= sizeof(host)) {
                    result = BRUCE_ERR_INVALID_ARGUMENT;
                } else if (udp) {
                    result = nc_app__udp_client(host, port, &opts);
                } else {
                    opts.timeout_ms =
                        timeout_seconds > 0 ? (uint32_t)timeout_seconds * 1000u : NC_APP_DEFAULT_CONNECT_TIMEOUT_MS;
                    result = nc_app__client(host, port, &opts);
                }
            }
        }

        if (opts.dump_file != BRUCE_FILE_ID_INVALID) (void)storage__close(opts.dump_file);
    }
    ap_free(root);
    return result;
}
