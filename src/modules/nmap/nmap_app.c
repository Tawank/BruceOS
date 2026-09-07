#include "nmap_app.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/icmp.h"
#include "core_sdk/memory.h"
#include "core_sdk/net.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tcp.h"
#include "core_sdk/udp.h"
#include "core_sdk/wifi.h"

#define NMAP_APP_EXIT_BYTE 0x1du
#define NMAP_APP_EXIT_BYTE_ALT 0x04u
#define NMAP_APP_HOST_TEXT_MAX 64u
#define NMAP_APP_MAX_HOSTS 256u
#define NMAP_APP_MAX_PORTS 2048u
#define NMAP_APP_MAX_PROBES 8192u
#define NMAP_APP_BANNER_MAX 96u
#define NMAP_APP_REPORT_LINE_MAX 224u

/* Common ports, roughly in order of how often they matter on a pentest --
 * this build's stand-in for real nmap's ~1000-entry frequency-ranked
 * default list, sized for a single Wi-Fi radio rather than a wired NIC.
 * Also what --top-ports N draws its first N entries from. */
static const uint16_t nmap_app__top_ports[] = {
    80,  443,  22,   21,  23,  25,   53,   110,  139,   143,  445,  3389, 3306, 5900, 8080,
    8443, 8000, 993, 995, 111, 135,  1723, 5000, 6379, 27017, 161,  123,  179,  500,  8888,
};
#define NMAP_APP_TOP_PORTS_COUNT (sizeof(nmap_app__top_ports) / sizeof(nmap_app__top_ports[0]))

/* -T0 (paranoid) .. -T5 (insane): approximates real nmap's timing intent by
 * scaling this build's own per-probe timeout/pacing -- not a reimplementation
 * of nmap's actual congestion-control timing engine. */
typedef struct {
    uint32_t timeout_ms;
    uint32_t interval_ms;
} nmap_app__timing_t;

static const nmap_app__timing_t nmap_app__timings[6] = {
    {2000u, 1000u}, /* T0 */
    {1500u, 500u},  /* T1 */
    {1000u, 200u},  /* T2 */
    {500u, 0u},     /* T3 (default) */
    {300u, 0u},     /* T4 */
    {150u, 0u},     /* T5 */
};

typedef struct {
    bool discovery_only;   /* -sn */
    bool skip_discovery;   /* -Pn */
    bool udp;              /* -sU */
    bool banner_grab;      /* -sV -- a raw banner, not real probe/signature-matched version detection */
    bool verbose;          /* -v */
    uint32_t probe_timeout_ms;
    uint32_t probe_interval_ms;
    bruce_file_id_t output_file; /* -oN, BRUCE_FILE_ID_INVALID when not writing to a file */
} nmap_app__options_t;

typedef struct {
    char text[NMAP_APP_HOST_TEXT_MAX];
} nmap_app__host_t;

typedef enum {
    NMAP_APP_PORT_OPEN,
    NMAP_APP_PORT_CLOSED,
    NMAP_APP_PORT_FILTERED,
    NMAP_APP_PORT_OPEN_OR_FILTERED,
} nmap_app__port_state_t;

static const char *nmap_app__port_state_name(nmap_app__port_state_t state) {
    switch (state) {
        case NMAP_APP_PORT_OPEN: return "open";
        case NMAP_APP_PORT_CLOSED: return "closed";
        case NMAP_APP_PORT_FILTERED: return "filtered";
        default: return "open|filtered";
    }
}

/* Prints one line to the terminal and, when -oN named a file, appends the
 * same line there too -- same "mirror everything to a log" idea as nc's -o,
 * just plain text instead of a hex dump. */
static void nmap_app__report(const nmap_app__options_t *opts, const char *format, ...) {
    char line[NMAP_APP_REPORT_LINE_MAX];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length <= 0) return;
    stdio__printf("%s", line);
    if (opts->output_file != BRUCE_FILE_ID_INVALID) {
        size_t written = 0;
        size_t size = (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1;
        (void)storage__write(opts->output_file, line, size, &written);
    }
}

static bool nmap_app__cancelled(void) {
    char input[1];
    size_t input_size = 0;
    bruce_result_t result = stdio__read(input, sizeof(input), 0, &input_size);
    if (result == BRUCE_ERR_NOT_FOUND) return true;
    return result == BRUCE_OK && input_size > 0 &&
           (input[0] == (char)NMAP_APP_EXIT_BYTE || input[0] == (char)NMAP_APP_EXIT_BYTE_ALT);
}

static bool nmap_app__parse_port(const char *text, uint16_t *out_port) {
    if (text == NULL || out_port == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT16_MAX) return false;
    *out_port = (uint16_t)value;
    return true;
}

/* A single port ("80") or a "low-high" range ("20-1024"); nmap_app__parse_ports()
 * below applies this to each comma-separated token to also support lists
 * ("22,80,8000-8100"), matching real nmap's -p syntax. */
static bool nmap_app__parse_port_range(const char *text, uint16_t *out_low, uint16_t *out_high) {
    const char *dash = strchr(text, '-');
    if (dash == NULL) {
        uint16_t port;
        if (!nmap_app__parse_port(text, &port)) return false;
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
    if (!nmap_app__parse_port(low_text, &low)) return false;
    if (!nmap_app__parse_port(dash + 1, &high)) return false;
    if (low > high) return false;
    *out_low = low;
    *out_high = high;
    return true;
}

static bool nmap_app__parse_ports(const char *text, uint16_t *out_ports, size_t capacity, size_t *out_count) {
    *out_count = 0;
    const char *cursor = text;
    while (cursor != NULL && *cursor != '\0') {
        const char *comma = strchr(cursor, ',');
        size_t token_len = comma != NULL ? (size_t)(comma - cursor) : strlen(cursor);
        char token[16];
        if (token_len == 0 || token_len >= sizeof(token)) return false;
        memcpy(token, cursor, token_len);
        token[token_len] = '\0';

        uint16_t low, high;
        if (!nmap_app__parse_port_range(token, &low, &high)) return false;
        for (uint32_t port = low; port <= high; ++port) {
            if (*out_count >= capacity) return false;
            out_ports[(*out_count)++] = (uint16_t)port;
        }
        cursor = comma != NULL ? comma + 1 : NULL;
    }
    return *out_count > 0;
}

/* Expands one target expression into a host list. Three forms, matching what
 * real nmap accepts for the common (non-hostgroup-file) case:
 *   - a single host: a literal IP or a hostname, copied through unchanged
 *     (resolution happens later, in icmp__ping()/tcp__connect() themselves)
 *   - a CIDR range: "192.168.1.0/24" -- needs a literal IPv4 base, since
 *     there's no such thing as a CIDR range of hostnames
 *   - a last-octet range: "192.168.1.1-254", the common shorthand for
 *     scanning part of one /24 without CIDR math
 */
static bool nmap_app__expand_targets(
    const char *text, nmap_app__host_t *out_hosts, size_t capacity, size_t *out_count, const char **out_error
) {
    *out_count = 0;
    *out_error = NULL;

    const char *slash = strchr(text, '/');
    if (slash != NULL) {
        char base_text[BRUCE_NET_IPV4_TEXT_MAX];
        size_t base_len = (size_t)(slash - text);
        if (base_len == 0 || base_len >= sizeof(base_text)) {
            *out_error = "invalid CIDR base address";
            return false;
        }
        memcpy(base_text, text, base_len);
        base_text[base_len] = '\0';
        uint32_t base = 0;
        if (net__parse_ipv4(base_text, &base) != BRUCE_OK) {
            *out_error = "a CIDR target needs a literal IPv4 base address, not a hostname";
            return false;
        }
        char *end = NULL;
        unsigned long prefix = strtoul(slash + 1, &end, 10);
        if (*end != '\0' || prefix > 32) {
            *out_error = "invalid CIDR prefix length (0-32)";
            return false;
        }
        uint32_t mask = prefix == 0 ? 0u : (uint32_t)(0xFFFFFFFFu << (32u - (uint32_t)prefix));
        uint32_t first = base & mask;
        uint32_t last = first | ~mask;
        uint64_t count = (uint64_t)last - (uint64_t)first + 1u;
        if (count > capacity) {
            *out_error = "CIDR range too large for one scan -- narrow the prefix";
            return false;
        }
        for (uint32_t address = first;; ++address) {
            net__format_ipv4(address, out_hosts[*out_count].text, sizeof(out_hosts[0].text));
            (*out_count)++;
            if (address == last) break;
        }
        return true;
    }

    const char *last_dot = strrchr(text, '.');
    const char *dash = last_dot != NULL ? strchr(last_dot, '-') : NULL;
    if (last_dot != NULL && dash != NULL) {
        size_t prefix_len = (size_t)(last_dot - text) + 1; /* keep the dot */
        if (prefix_len >= NMAP_APP_HOST_TEXT_MAX) {
            *out_error = "target is too long";
            return false;
        }
        char low_text[8];
        size_t low_len = (size_t)(dash - (last_dot + 1));
        if (low_len == 0 || low_len >= sizeof(low_text)) {
            *out_error = "invalid host range";
            return false;
        }
        memcpy(low_text, last_dot + 1, low_len);
        low_text[low_len] = '\0';
        char *end = NULL;
        unsigned long low = strtoul(low_text, &end, 10);
        if (*end != '\0' || low > 255) {
            *out_error = "invalid host range";
            return false;
        }
        unsigned long high = strtoul(dash + 1, &end, 10);
        if (*end != '\0' || high > 255 || high < low) {
            *out_error = "invalid host range";
            return false;
        }
        if (high - low + 1 > capacity) {
            *out_error = "host range too large for one scan";
            return false;
        }
        for (unsigned long n = low; n <= high; ++n) {
            snprintf(out_hosts[*out_count].text, sizeof(out_hosts[0].text), "%.*s%lu", (int)prefix_len, text, n);
            (*out_count)++;
        }
        return true;
    }

    if (strlen(text) >= NMAP_APP_HOST_TEXT_MAX) {
        *out_error = "target is too long";
        return false;
    }
    snprintf(out_hosts[0].text, sizeof(out_hosts[0].text), "%s", text);
    *out_count = 1;
    return true;
}

/* "Up" without raw ICMP visibility means one of two things happened: a real
 * ICMP echo reply came back, or a TCP probe got an active refusal
 * (BRUCE_ERR_IO from tcp__connect() -- only possible if something answered).
 * Only silence on every attempt counts as "no reply". */
static bool
nmap_app__host_is_up(const char *host, const nmap_app__options_t *opts, bool *out_has_latency, uint32_t *out_round_trip_ms) {
    *out_has_latency = false;
    if (icmp__ping(host, opts->probe_timeout_ms, out_round_trip_ms) == BRUCE_OK) {
        *out_has_latency = true;
        return true;
    }
    static const uint16_t fallback_ports[] = {80, 443, 22};
    for (size_t i = 0; i < sizeof(fallback_ports) / sizeof(fallback_ports[0]); ++i) {
        bruce_tcp_id_t socket = BRUCE_TCP_ID_INVALID;
        bruce_result_t result = tcp__connect(host, fallback_ports[i], opts->probe_timeout_ms, &socket);
        if (result == BRUCE_OK) {
            (void)tcp__close(socket);
            return true;
        }
        if (result == BRUCE_ERR_IO) return true;
    }
    return false;
}

static nmap_app__port_state_t nmap_app__probe_tcp_port(
    const char *host, uint16_t port, uint32_t timeout_ms, char *out_banner, size_t banner_capacity, bool grab_banner
) {
    if (out_banner != NULL && banner_capacity > 0) out_banner[0] = '\0';
    bruce_tcp_id_t socket = BRUCE_TCP_ID_INVALID;
    bruce_result_t result = tcp__connect(host, port, timeout_ms, &socket);
    if (result != BRUCE_OK) return result == BRUCE_ERR_IO ? NMAP_APP_PORT_CLOSED : NMAP_APP_PORT_FILTERED;

    if (grab_banner && out_banner != NULL && banner_capacity > 0) {
        /* Best-effort only: services that announce themselves unprompted
         * (SSH/FTP/SMTP/...) show up here; anything that waits for the
         * client to speak first reads as no banner, not as closed. */
        size_t received = 0;
        if (tcp__read(socket, out_banner, banner_capacity - 1, 300, &received) == BRUCE_OK && received > 0) {
            out_banner[received] = '\0';
            for (size_t i = 0; i < received; ++i) {
                if (out_banner[i] == '\r' || out_banner[i] == '\n') {
                    out_banner[i] = '\0';
                    break;
                }
            }
        }
    }
    (void)tcp__close(socket);
    return NMAP_APP_PORT_OPEN;
}

/* UDP has no handshake: udp__open() always "succeeds" locally, so openness
 * can only be inferred from getting an actual reply. Without raw ICMP,
 * silence is genuinely ambiguous between open and filtered -- real nmap
 * reports the same "open|filtered" bucket in exactly this situation (no
 * response, and no ICMP port-unreachable visible to tell the two apart). */
static nmap_app__port_state_t nmap_app__probe_udp_port(const char *host, uint16_t port, uint32_t timeout_ms) {
    bruce_udp_id_t socket = BRUCE_UDP_ID_INVALID;
    if (udp__open(0, &socket) != BRUCE_OK) return NMAP_APP_PORT_FILTERED;
    static const char probe_byte = '\0';
    size_t sent = 0;
    if (udp__send_to(socket, host, port, &probe_byte, 1, timeout_ms, &sent) != BRUCE_OK || sent == 0) {
        (void)udp__close(socket);
        return NMAP_APP_PORT_FILTERED;
    }
    char buffer[64];
    size_t received = 0;
    bruce_udp_endpoint_t sender = {0};
    bruce_result_t result = udp__receive_from(socket, buffer, sizeof(buffer), timeout_ms, &received, &sender);
    (void)udp__close(socket);
    return result == BRUCE_OK && received > 0 ? NMAP_APP_PORT_OPEN : NMAP_APP_PORT_OPEN_OR_FILTERED;
}

static bruce_result_t nmap_app__run(
    const nmap_app__host_t *hosts, size_t host_count, const uint16_t *ports, size_t port_count,
    const nmap_app__options_t *opts
) {
    if (!wifi__is_connected()) {
        stdio__printf("nmap: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }

    unsigned int hosts_up = 0;
    for (size_t h = 0; h < host_count; ++h) {
        if (nmap_app__cancelled()) {
            nmap_app__report(opts, "Scan cancelled.\n");
            return BRUCE_OK;
        }
        const char *host = hosts[h].text;
        bool has_latency = false;
        uint32_t round_trip_ms = 0;
        bool up = opts->skip_discovery || nmap_app__host_is_up(host, opts, &has_latency, &round_trip_ms);
        if (!up) {
            nmap_app__report(opts, "Host %s seems down (use -Pn to scan it anyway).\n", host);
            continue;
        }
        ++hosts_up;
        if (opts->skip_discovery) {
            nmap_app__report(opts, "Host %s: assumed up (-Pn).\n", host);
        } else if (has_latency) {
            nmap_app__report(opts, "Host %s is up (%lums latency).\n", host, (unsigned long)round_trip_ms);
        } else {
            nmap_app__report(opts, "Host %s is up (no ICMP reply, but answered on TCP).\n", host);
        }
        if (opts->discovery_only) continue;

        unsigned int hidden_count = 0;
        for (size_t p = 0; p < port_count; ++p) {
            if (nmap_app__cancelled()) {
                nmap_app__report(opts, "Scan cancelled.\n");
                return BRUCE_OK;
            }
            char banner[NMAP_APP_BANNER_MAX];
            nmap_app__port_state_t state =
                opts->udp ? nmap_app__probe_udp_port(host, ports[p], opts->probe_timeout_ms)
                          : nmap_app__probe_tcp_port(
                                host, ports[p], opts->probe_timeout_ms, banner, sizeof(banner), opts->banner_grab
                            );
            bool interesting = state == NMAP_APP_PORT_OPEN || state == NMAP_APP_PORT_OPEN_OR_FILTERED;
            if (interesting) {
                if (!opts->udp && opts->banner_grab && banner[0] != '\0') {
                    nmap_app__report(
                        opts, "%u/%s %-13s banner: %s\n", (unsigned int)ports[p], opts->udp ? "udp" : "tcp",
                        nmap_app__port_state_name(state), banner
                    );
                } else {
                    nmap_app__report(
                        opts, "%u/%s %s\n", (unsigned int)ports[p], opts->udp ? "udp" : "tcp",
                        nmap_app__port_state_name(state)
                    );
                }
            } else if (opts->verbose) {
                nmap_app__report(
                    opts, "%u/%s %s\n", (unsigned int)ports[p], opts->udp ? "udp" : "tcp",
                    nmap_app__port_state_name(state)
                );
            } else {
                ++hidden_count;
            }
            if (runtime__delay(opts->probe_interval_ms > 0 ? opts->probe_interval_ms : 1) != BRUCE_OK) {
                return BRUCE_ERR_CANCELLED;
            }
        }
        if (!opts->verbose && hidden_count > 0) {
            nmap_app__report(opts, "Not shown: %u closed/filtered port(s)\n", hidden_count);
        }
    }

    nmap_app__report(opts, "Scan done: %u host(s) scanned, %u up.\n", (unsigned int)host_count, hosts_up);
    return BRUCE_OK;
}

static int nmap_app__gui(void) {
    char target[NMAP_APP_HOST_TEXT_MAX];
    bruce_result_t target_result =
        dialog__text_input("nmap", "Target (host, CIDR, or a.b.c.x-y)", NULL, false, target, sizeof(target));
    if (target_result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (target_result != BRUCE_OK) return target_result;
    if (target[0] == '\0') {
        (void)dialog__message(BRUCE_DIALOG_ERROR, "nmap", "Target is required");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    const bruce_dialog_choice_t choices[] = {
        {.label = "Host discovery only", .value = "sn",   .icon_name = "radar"  },
        {.label = "Full scan",           .value = "full", .icon_name = "magnify"},
        {.label = "Back",                .value = "back"                       },
    };
    size_t selected = 0;
    bruce_result_t choice_result = dialog__choice_launcher("nmap", NULL, choices, 3, &selected);
    if (choice_result == BRUCE_ERR_CANCELLED) return BRUCE_OK;
    if (choice_result != BRUCE_OK) return choice_result;
    if (strcmp(choices[selected].value, "back") == 0) return BRUCE_OK;

    char command[NMAP_APP_HOST_TEXT_MAX + 32];
    bool discovery_only = strcmp(choices[selected].value, "sn") == 0;
    snprintf(command, sizeof(command), "GUI=1 terminal nmap %s-v %s", discovery_only ? "-sn " : "", target);
    return app_runner__run_command(command, BRUCE_LAUNCH_FOREGROUND);
}

int nmap_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(
        root,
        "nmap-style host discovery and TCP/UDP port scanner. `nmap <target>` scans the built-in "
        "top-ports list; <target> may be a single host, a CIDR range (\"192.168.1.0/24\"), or a "
        "last-octet range (\"192.168.1.1-254\"). This is a TCP-connect scanner, not a raw-packet one: "
        "no -sS/-sF/-sX stealth scans, no OS fingerprinting (-O), no NSE scripts -- see each option's "
        "help for exactly what it does instead."
    );
    ap_set_version(root, "nmap (BruceOS) 1.0");

    ap_add_flag(root, "sn");
    ap_set_opt_help(root, "sn", "Host discovery only -- report which targets are up, scan no ports");
    ap_add_flag(root, "Pn");
    ap_set_opt_help(root, "Pn", "Skip host discovery -- treat every target as up");
    ap_add_str_opt(root, "p", "");
    ap_set_opt_help(
        root, "p",
        "Ports to scan: a single port, a range (\"20-1024\"), or a comma list of either "
        "(\"22,80,8000-8100\"). Default: the built-in top-ports list"
    );
    ap_add_int_opt(root, "top-ports", 0);
    ap_set_opt_help(
        root, "top-ports", "Scan the N most common ports from the built-in list, instead of the default or -p"
    );
    ap_add_flag(root, "sT");
    ap_set_opt_help(root, "sT", "TCP connect scan (the default -- accepted for command-line familiarity)");
    ap_add_flag(root, "sU");
    ap_set_opt_help(
        root, "sU",
        "UDP scan instead of TCP. Without a reply, a port is reported open|filtered -- the same "
        "ambiguity real nmap has when it can't see an ICMP port-unreachable reply either"
    );
    ap_add_flag(root, "sV");
    ap_set_opt_help(
        root, "sV",
        "Grab whatever an open TCP port sends unprompted and show it as a raw banner -- not real "
        "nmap's probe-database service/version match"
    );
    ap_add_int_opt(root, "T", 3);
    ap_set_opt_help(
        root, "T",
        "Timing template 0 (paranoid/slow) to 5 (insane/fast), default 3 -- approximates real nmap's "
        "timing intent via this build's own probe timeout/pacing"
    );
    ap_add_str_opt(root, "oN", "");
    ap_set_opt_help(root, "oN", "Also write plain-text output to <file>");
    ap_add_flag(root, "v");
    ap_set_opt_help(root, "v", "Verbose: also report closed/filtered ports explicitly");
    ap_add_flag(root, "verbose");
    ap_set_opt_help(root, "verbose", "Alias for -v");
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
        return nmap_app__gui();
    }

    bool discovery_only = ap_found(root, "sn");
    bool skip_discovery = ap_found(root, "Pn");
    bool udp = ap_found(root, "sU");
    bool banner_grab = ap_found(root, "sV");
    bool verbose = ap_found(root, "v") || ap_found(root, "verbose");
    int timing = ap_found(root, "T") ? ap_get_int_value(root, "T") : 3;
    const char *port_text = ap_found(root, "p") ? ap_get_str_value(root, "p") : NULL;
    if (port_text != NULL && port_text[0] == '\0') port_text = NULL;
    int top_ports = ap_found(root, "top-ports") ? ap_get_int_value(root, "top-ports") : 0;
    const char *output_path = ap_found(root, "oN") ? ap_get_str_value(root, "oN") : NULL;
    if (output_path != NULL && output_path[0] == '\0') output_path = NULL;
    const char *target = ap_count_args(root) > 0 ? ap_get_arg_at_index(root, 0) : NULL;

    int result;
    if (target == NULL) {
        stdio__printf("nmap: usage: nmap [options] <target>\n");
        ap_print_help(root);
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (timing < 0 || timing > 5) {
        stdio__printf("nmap: -T must be 0-5\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (discovery_only && skip_discovery) {
        stdio__printf("nmap: -sn (discovery only) and -Pn (skip discovery) cannot be combined\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (discovery_only && (port_text != NULL || top_ports > 0 || udp || banner_grab)) {
        stdio__printf("nmap: -sn (discovery only) cannot be combined with -p/--top-ports/-sU/-sV\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (port_text != NULL && top_ports > 0) {
        stdio__printf("nmap: -p and --top-ports cannot be combined\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else if (top_ports < 0) {
        stdio__printf("nmap: --top-ports must be positive\n");
        result = BRUCE_ERR_INVALID_ARGUMENT;
    } else {
        nmap_app__host_t *hosts = memory__malloc(NMAP_APP_MAX_HOSTS * sizeof(nmap_app__host_t));
        uint16_t *ports = memory__malloc(NMAP_APP_MAX_PORTS * sizeof(uint16_t));
        if (hosts == NULL || ports == NULL) {
            memory__free(hosts);
            memory__free(ports);
            ap_free(root);
            return BRUCE_ERR_NO_MEMORY;
        }

        size_t host_count = 0;
        size_t port_count = 0;
        const char *expand_error = NULL;
        if (!nmap_app__expand_targets(target, hosts, NMAP_APP_MAX_HOSTS, &host_count, &expand_error)) {
            stdio__printf("nmap: %s\n", expand_error != NULL ? expand_error : "invalid target");
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else if (!discovery_only && port_text != NULL &&
                   !nmap_app__parse_ports(port_text, ports, NMAP_APP_MAX_PORTS, &port_count)) {
            stdio__printf("nmap: invalid or too many ports in -p (max %u)\n", (unsigned int)NMAP_APP_MAX_PORTS);
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            if (!discovery_only && port_text == NULL) {
                size_t count = top_ports > 0 ? (size_t)top_ports : NMAP_APP_TOP_PORTS_COUNT;
                if (count > NMAP_APP_TOP_PORTS_COUNT) count = NMAP_APP_TOP_PORTS_COUNT;
                for (size_t i = 0; i < count; ++i) ports[port_count++] = nmap_app__top_ports[i];
            }
            size_t probes = host_count * (port_count > 0 ? port_count : 1);
            if (probes > NMAP_APP_MAX_PROBES) {
                stdio__printf(
                    "nmap: %u host(s) x %u port(s) is too much for one scan (max %u probes) -- narrow "
                    "the target or port list\n",
                    (unsigned int)host_count, (unsigned int)port_count, (unsigned int)NMAP_APP_MAX_PROBES
                );
                result = BRUCE_ERR_INVALID_ARGUMENT;
            } else {
                nmap_app__options_t opts = {
                    .discovery_only = discovery_only,
                    .skip_discovery = skip_discovery,
                    .udp = udp,
                    .banner_grab = banner_grab,
                    .verbose = verbose,
                    .probe_timeout_ms = nmap_app__timings[timing].timeout_ms,
                    .probe_interval_ms = nmap_app__timings[timing].interval_ms,
                    .output_file = BRUCE_FILE_ID_INVALID,
                };

                bool output_ok = true;
                if (output_path != NULL) {
                    bruce_result_t open_result = storage__open(
                        output_path,
                        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
                        &opts.output_file
                    );
                    if (open_result != BRUCE_OK) {
                        stdio__printf("nmap: -oN: could not open '%s' for writing (%d)\n", output_path, open_result);
                        result = open_result;
                        output_ok = false;
                    }
                }
                if (output_ok) {
                    result = nmap_app__run(hosts, host_count, ports, port_count, &opts);
                    if (opts.output_file != BRUCE_FILE_ID_INVALID) (void)storage__close(opts.output_file);
                }
            }
        }

        memory__free(hosts);
        memory__free(ports);
    }
    ap_free(root);
    return result;
}
