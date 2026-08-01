#include "ssh_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/dialog.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/ssh.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/wifi.h"

#define SSH_APP_BUFFER_SIZE 256u
#define SSH_APP_IO_TIMEOUT_MS 20u
#define SSH_APP_EXIT_BYTE 0x1du
#define SSH_APP_HOST_MAX 64u
#define SSH_APP_USERNAME_MAX 64u
#define SSH_APP_PASSWORD_MAX 128u
#define SSH_APP_KEY_MAX 96u
#define SSH_APP_DEFAULT_COLUMNS 80u
#define SSH_APP_DEFAULT_ROWS 24u
#define SSH_APP_KNOWN_HOSTS_PATH "/ssh_known_hosts"
#define SSH_APP_KNOWN_HOSTS_MAX_BYTES 4096u

static bool ssh_app__parse_port(const char *text, uint16_t *out_port) {
    if (text == NULL || out_port == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT16_MAX) return false;
    *out_port = (uint16_t)value;
    return true;
}

static void ssh_app__hex_encode(const uint8_t *bytes, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0Fu];
    }
    out[size * 2] = '\0';
}

static int ssh_app__hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ssh_app__hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_size) {
    if (hex_len != out_size * 2) return false;
    for (size_t i = 0; i < out_size; ++i) {
        int hi = ssh_app__hex_value(hex[i * 2]);
        int lo = ssh_app__hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* known_hosts entries are "<host>|<port> <64 hex chars>\n". '|' (rather than
 * ':') separates host from port so IPv6 literals cannot be confused with it. */
static bruce_result_t ssh_app__read_known_hosts(char *buffer, size_t capacity, size_t *out_size) {
    *out_size = 0;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(SSH_APP_KNOWN_HOSTS_PATH, BRUCE_STORAGE_OPEN_READ, &file);
    if (result == BRUCE_ERR_NOT_FOUND) return BRUCE_OK;
    if (result != BRUCE_OK) return result;

    size_t total = 0;
    while (total < capacity) {
        size_t chunk = 0;
        result = storage__read(file, buffer + total, capacity - total, &chunk);
        if (result != BRUCE_OK) {
            storage__close(file);
            return result;
        }
        if (chunk == 0) break;
        total += chunk;
    }
    storage__close(file);
    *out_size = total;
    return BRUCE_OK;
}

static bool ssh_app__find_known_fingerprint(
    const char *buffer, size_t size, const char *key, uint8_t out_fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
) {
    size_t key_len = strlen(key);
    size_t pos = 0;
    while (pos < size) {
        const char *line = buffer + pos;
        const void *newline = memchr(line, '\n', size - pos);
        size_t line_len = newline != NULL ? (size_t)((const char *)newline - line) : size - pos;
        const void *space = memchr(line, ' ', line_len);
        if (space != NULL) {
            size_t line_key_len = (size_t)((const char *)space - line);
            size_t hex_len = line_len - line_key_len - 1;
            if (line_key_len == key_len && memcmp(line, key, key_len) == 0 &&
                ssh_app__hex_decode(
                    (const char *)space + 1, hex_len, out_fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE
                ))
                return true;
        }
        pos += line_len + 1;
    }
    return false;
}

/* Rewrites the known_hosts file with any prior entry for `key` replaced.
 * Entries beyond SSH_APP_KNOWN_HOSTS_MAX_BYTES are silently dropped; this
 * store is meant for a handful of personally-managed hosts, not a fleet. */
static bruce_result_t ssh_app__store_known_fingerprint(
    const char *key, const uint8_t fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
) {
    char *existing = memory__malloc(SSH_APP_KNOWN_HOSTS_MAX_BYTES);
    char *rebuilt = memory__malloc(SSH_APP_KNOWN_HOSTS_MAX_BYTES);
    if (existing == NULL || rebuilt == NULL) {
        memory__free(existing);
        memory__free(rebuilt);
        return BRUCE_ERR_NO_MEMORY;
    }

    size_t existing_size = 0;
    bruce_result_t result = ssh_app__read_known_hosts(existing, SSH_APP_KNOWN_HOSTS_MAX_BYTES, &existing_size);
    if (result != BRUCE_OK) {
        memory__free(existing);
        memory__free(rebuilt);
        return result;
    }

    size_t key_len = strlen(key);
    size_t rebuilt_size = 0;
    size_t pos = 0;
    while (pos < existing_size) {
        const char *line = existing + pos;
        const void *newline = memchr(line, '\n', existing_size - pos);
        size_t line_len = newline != NULL ? (size_t)((const char *)newline - line) : existing_size - pos;
        const void *space = memchr(line, ' ', line_len);
        bool is_match =
            space != NULL && (size_t)((const char *)space - line) == key_len && memcmp(line, key, key_len) == 0;
        if (!is_match && line_len > 0 && rebuilt_size + line_len + 1 <= SSH_APP_KNOWN_HOSTS_MAX_BYTES) {
            memcpy(rebuilt + rebuilt_size, line, line_len);
            rebuilt_size += line_len;
            rebuilt[rebuilt_size++] = '\n';
        }
        pos += line_len + 1;
    }
    memory__free(existing);

    char hex[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    ssh_app__hex_encode(fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE, hex);
    char new_line[SSH_APP_KEY_MAX + sizeof(hex) + 2];
    int new_line_len = snprintf(new_line, sizeof(new_line), "%s %s\n", key, hex);
    if (new_line_len > 0 && rebuilt_size + (size_t)new_line_len <= SSH_APP_KNOWN_HOSTS_MAX_BYTES) {
        memcpy(rebuilt + rebuilt_size, new_line, (size_t)new_line_len);
        rebuilt_size += (size_t)new_line_len;
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    result = storage__open(
        SSH_APP_KNOWN_HOSTS_PATH,
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result == BRUCE_OK) {
        size_t written = 0;
        result = storage__write(file, rebuilt, rebuilt_size, &written);
        storage__close(file);
    }
    memory__free(rebuilt);
    return result;
}

/* TOFU (trust-on-first-use) host key check backed by a persistent
 * known_hosts store: a first-seen key is shown to the user for out-of-band
 * verification and, once accepted, remembered; a later mismatch is a loud
 * warning (possible MITM or reinstalled host) rather than a silent accept. */
static bruce_result_t ssh_app__verify_host_key(bruce_ssh_id_t session, const char *host, uint16_t port) {
    uint8_t fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bruce_result_t result = ssh__host_key_sha256(session, fingerprint);
    if (result != BRUCE_OK) return result;

    char hex[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    ssh_app__hex_encode(fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE, hex);

    char key[SSH_APP_KEY_MAX];
    snprintf(key, sizeof(key), "%s|%u", host, (unsigned int)port);

    char *known_hosts = memory__malloc(SSH_APP_KNOWN_HOSTS_MAX_BYTES);
    if (known_hosts == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t known_hosts_size = 0;
    result = ssh_app__read_known_hosts(known_hosts, SSH_APP_KNOWN_HOSTS_MAX_BYTES, &known_hosts_size);
    if (result != BRUCE_OK) {
        memory__free(known_hosts);
        return result;
    }
    uint8_t stored[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bool known = ssh_app__find_known_fingerprint(known_hosts, known_hosts_size, key, stored);
    memory__free(known_hosts);

    if (known && memcmp(stored, fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE) == 0) {
        stdio__printf("Host key fingerprint SHA256:%s matches the saved known_hosts entry.\n", hex);
        return ssh__verify_host_key_sha256(session, fingerprint);
    }

    size_t selected = 1;
    bruce_dialog_choice_t choices[2] = {
        {.label = "Trust & connect", .value = "trust"},
        {.label = "Abort",           .value = "abort"},
    };
    bruce_result_t choice_result;
    if (known) {
        stdio__printf(
            "WARNING: host key for %s changed! New fingerprint SHA256:%s\n"
            "This can mean someone is intercepting the connection, or the host was reinstalled.\n",
            key, hex
        );
        choices[0].label = "Trust new key & continue";
        choice_result =
            dialog__choice("SSH host key changed", "The remote host key does not match the saved one.", choices,
                            2, &selected, NULL);
    } else {
        stdio__printf("The authenticity of host '%s' can't be established.\nSHA256 fingerprint: %s\n", key, hex);
        choice_result = dialog__choice(
            "Unknown SSH host key", "Verify this fingerprint out-of-band before trusting it.", choices, 2,
            &selected, NULL
        );
    }
    if (choice_result != BRUCE_OK || selected != 0) return BRUCE_ERR_PERMISSION;

    result = ssh__verify_host_key_sha256(session, fingerprint);
    if (result != BRUCE_OK) return result;
    return ssh_app__store_known_fingerprint(key, fingerprint);
}

static bruce_result_t ssh_app__forward_stdin(bruce_ssh_id_t session, bool *out_exit) {
    char input[SSH_APP_BUFFER_SIZE];
    size_t input_size = 0;
    bruce_result_t result = stdio__read(input, sizeof(input), 0, &input_size);
    if (result == BRUCE_ERR_TIMEOUT) return BRUCE_OK;
    if (result != BRUCE_OK) return result;

    char *exit_byte = memchr(input, SSH_APP_EXIT_BYTE, input_size);
    size_t send_size = exit_byte != NULL ? (size_t)(exit_byte - input) : input_size;
    size_t total = 0;
    while (total < send_size) {
        size_t sent = 0;
        result = ssh__write(session, input + total, send_size - total, 1000, &sent);
        if (result != BRUCE_OK) return result;
        if (sent == 0) return BRUCE_ERR_IO;
        total += sent;
    }
    *out_exit = exit_byte != NULL;
    return BRUCE_OK;
}

static bruce_result_t ssh_app__session(bruce_ssh_id_t session, bool *out_local_exit) {
    *out_local_exit = false;
    char received[SSH_APP_BUFFER_SIZE + 1];
    for (;;) {
        size_t received_size = 0;
        bruce_result_t result = ssh__read(
            session, false, received, SSH_APP_BUFFER_SIZE, SSH_APP_IO_TIMEOUT_MS, &received_size
        );
        if (result == BRUCE_OK) {
            if (received_size == 0) return BRUCE_OK;
            received[received_size] = '\0';
            (void)stdio__write(received, received_size);
        } else if (result != BRUCE_ERR_TIMEOUT) {
            return result;
        }

        bool exit_requested = false;
        result = ssh_app__forward_stdin(session, &exit_requested);
        if (result == BRUCE_ERR_NOT_FOUND || exit_requested) {
            *out_local_exit = true;
            return BRUCE_OK;
        }
        if (result != BRUCE_OK) return result;
        if (runtime__delay(1) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
}

/* TODO(remove): temporary diagnostic for tracking down BRUCE_ERR_NO_MEMORY
 * failures from libssh2_session_init() on PSRAM-less boards. */
static void ssh_app__log_heap_state(const char *label) {
    bruce_memory_stats_t stats;
    if (memory__get_stats(&stats) != BRUCE_OK) return;
    stdio__printf(
        "[ssh mem] %s: internal free=%u largest=%u (total=%u); dram(8-bit) free=%u largest=%u (total=%u)",
        label, (unsigned)stats.internal_free, (unsigned)stats.internal_largest_block,
        (unsigned)stats.internal_total, (unsigned)stats.dram_8bit_free, (unsigned)stats.dram_8bit_largest_block,
        (unsigned)stats.dram_8bit_total
    );
    if (stats.psram_total > 0) {
        stdio__printf(
            ", psram free=%u largest=%u (total=%u)", (unsigned)stats.psram_free,
            (unsigned)stats.psram_largest_block, (unsigned)stats.psram_total
        );
    }
    stdio__printf("\n");
}

static bruce_result_t ssh_app__client(
    const char *host, uint16_t port, const char *username, const char *password
) {
    if (!wifi__is_connected()) {
        stdio__printf("SSH client: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    stdio__printf("Connecting to %s:%u...\n", host, (unsigned int)port);
    ssh_app__log_heap_state("before ssh__connect");
    bruce_ssh_id_t session = BRUCE_SSH_ID_INVALID;
    bruce_result_t result = ssh__connect(host, port, 10000, &session);
    ssh_app__log_heap_state("after ssh__connect");
    if (result != BRUCE_OK) {
        stdio__printf("SSH client: connection failed (%d)\n", result);
        return result;
    }

    result = ssh_app__verify_host_key(session, host, port);
    if (result != BRUCE_OK) {
        stdio__printf("SSH client: host key not verified, aborting (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }

    char password_buffer[SSH_APP_PASSWORD_MAX];
    const char *effective_password = password;
    if (password == NULL || password[0] == '\0') {
        char prompt[96];
        snprintf(prompt, sizeof(prompt), "Password for %s@%s", username, host);
        result = dialog__text_input("SSH", prompt, NULL, true, password_buffer, sizeof(password_buffer));
        if (result != BRUCE_OK) {
            (void)ssh__close(session);
            return result;
        }
        effective_password = password_buffer;
    }

    result = ssh__authenticate_password(session, username, effective_password, 10000);
    memset(password_buffer, 0, sizeof(password_buffer));
    if (result != BRUCE_OK) {
        stdio__printf("SSH client: authentication failed (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }

    result = ssh__open_shell(session, "xterm", SSH_APP_DEFAULT_COLUMNS, SSH_APP_DEFAULT_ROWS, 10000);
    if (result != BRUCE_OK) {
        stdio__printf("SSH client: failed to open shell (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }

    stdio__printf("Connected. Press Ctrl+] to close.\n");
    bool local_exit = false;
    result = ssh_app__session(session, &local_exit);
    (void)ssh__close(session);
    stdio__printf("\nConnection closed%s\n", result == BRUCE_OK ? "." : " with an error.");
    return result;
}

int ssh_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(
        root,
        "Open an interactive SSH session. Verifies the host key fingerprint against a persistent "
        "known_hosts store before authenticating, then forwards stdin/stdout to/from the remote "
        "shell. Press Ctrl+] to close."
    );
    ap_add_required_arg(root, "host", "Remote host name or address");
    ap_add_required_arg(root, "port", "Remote SSH port (1-65535)");
    ap_add_required_arg(root, "username", "Login username");
    ap_add_str_opt(root, "password", "");
    ap_set_opt_help(root, "password", "Password (prompted securely if omitted)");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION) ap_print_help(root);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }

    uint16_t port = 0;
    const char *port_text = ap_get_arg(root, "port");
    if (!ssh_app__parse_port(port_text, &port)) {
        ap_print_help(root);
        ap_free(root);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char host[SSH_APP_HOST_MAX];
    char username[SSH_APP_USERNAME_MAX];
    char password_copy[SSH_APP_PASSWORD_MAX];
    int host_len = snprintf(host, sizeof(host), "%s", ap_get_arg(root, "host"));
    int username_len = snprintf(username, sizeof(username), "%s", ap_get_arg(root, "username"));
    const char *password = ap_get_str_value(root, "password");
    snprintf(password_copy, sizeof(password_copy), "%s", password != NULL ? password : "");
    ap_free(root);

    if (host_len < 0 || (size_t)host_len >= sizeof(host) || username_len < 0 ||
        (size_t)username_len >= sizeof(username)) {
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    int result = ssh_app__client(host, port, username, password_copy);
    memset(password_copy, 0, sizeof(password_copy));
    return result;
}
