#include "ssh_app.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/dialog.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/ssh.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"
#include "core_sdk/wifi.h"

#define SSH_APP_BUFFER_SIZE 256u
#define SSH_APP_IO_TIMEOUT_MS 20u
#define SSH_APP_EXIT_BYTE 0x1du
#define SSH_APP_HOST_MAX 64u
#define SSH_APP_USERNAME_MAX 64u
#define SSH_APP_PASSWORD_MAX 128u
#define SSH_APP_PATH_MAX BRUCE_STORAGE_PATH_MAX
#define SSH_APP_COMMENT_MAX 64u
#define SSH_APP_KEY_MAX 96u
#define SSH_APP_DEFAULT_COLUMNS 80u
#define SSH_APP_DEFAULT_ROWS 24u
#define SSH_APP_DIRECTORY "/.ssh"
#define SSH_APP_KNOWN_HOSTS_PATH "/.ssh/known_hosts"
#define SSH_APP_LEGACY_KNOWN_HOSTS_PATH "/ssh_known_hosts"
#define SSH_APP_CONFIG_PATH "/.ssh/config"
#define SSH_APP_DEFAULT_ECDSA_IDENTITY_PATH "/.ssh/id_ecdsa"
#define SSH_APP_DEFAULT_ED25519_IDENTITY_PATH "/.ssh/id_ed25519"
#define SSH_APP_KNOWN_HOSTS_MAX_BYTES 4096u
#define SSH_APP_CONFIG_MAX_BYTES 4096u

typedef struct {
    char hostname[SSH_APP_HOST_MAX];
    char username[SSH_APP_USERNAME_MAX];
    char identity[SSH_APP_PATH_MAX];
    uint16_t port;
    bool hostname_set;
    bool username_set;
    bool identity_set;
    bool port_set;
} ssh_app__config_t;

static bool ssh_app__parse_port(const char *text, uint16_t *out_port) {
    if (text == NULL || out_port == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT16_MAX) return false;
    *out_port = (uint16_t)value;
    return true;
}

static bool ssh_app__host_pattern_matches(const char *pattern, const char *host) {
    while (*pattern != '\0') {
        if (*pattern == '*') {
            while (*pattern == '*') ++pattern;
            if (*pattern == '\0') return true;
            while (*host != '\0') {
                if (ssh_app__host_pattern_matches(pattern, host)) return true;
                ++host;
            }
            return false;
        }
        if (*host == '\0' ||
            (*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*host)))
            return false;
        ++pattern;
        ++host;
    }
    return *host == '\0';
}

static bool ssh_app__host_list_matches(char *patterns, const char *host) {
    bool matched = false;
    char *save = NULL;
    for (char *pattern = strtok_r(patterns, " \t", &save); pattern != NULL;
         pattern = strtok_r(NULL, " \t", &save)) {
        bool negated = pattern[0] == '!';
        if (negated) ++pattern;
        if (ssh_app__host_pattern_matches(pattern, host)) {
            if (negated) return false;
            matched = true;
        }
    }
    return matched;
}

static char *ssh_app__trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static void ssh_app__copy_config_value(char *out, size_t capacity, const char *value, bool *was_set) {
    if (*was_set) return;
    int length = snprintf(out, capacity, "%s", value);
    if (length >= 0 && (size_t)length < capacity) *was_set = true;
}

/* Prefers a PSRAM- or internal-RAM-backed memory__external allocation for
 * these multi-KiB scratch buffers (config/known_hosts parsing) so they don't
 * compete with the rest of the app for scarce internal RAM; falls back to
 * plain memory__malloc() when neither has room, mirroring terminal_app.c's
 * terminal__alloc_buffer(). memory__external_malloc_writable() never lands on
 * "swap", so this never pays for a flash erase just to discover the result
 * isn't directly writable. */
static bruce_result_t ssh_app__buffer_alloc(void **out_data, bool *out_external, size_t size) {
    const void *data = memory__external_malloc_writable(size);
    if (data != NULL) {
        *out_data = (void *)data;
        *out_external = true;
        return BRUCE_OK;
    }
    *out_data = memory__malloc(size);
    *out_external = false;
    return *out_data != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
}

static void ssh_app__buffer_free(void *data, bool external) {
    if (external) (void)memory__external_free(data);
    else memory__free(data);
}

static bruce_result_t ssh_app__load_config(const char *alias, ssh_app__config_t *config) {
    void *buffer_data = NULL;
    bool buffer_external = false;
    if (ssh_app__buffer_alloc(&buffer_data, &buffer_external, SSH_APP_CONFIG_MAX_BYTES + 1u) !=
        BRUCE_OK)
        return BRUCE_ERR_NO_MEMORY;
    char *buffer = buffer_data;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(SSH_APP_CONFIG_PATH, BRUCE_STORAGE_OPEN_READ, &file);
    if (result == BRUCE_ERR_NOT_FOUND) {
        ssh_app__buffer_free(buffer_data, buffer_external);
        return BRUCE_OK;
    }
    if (result != BRUCE_OK) {
        ssh_app__buffer_free(buffer_data, buffer_external);
        return result;
    }
    size_t size = 0;
    while (size < SSH_APP_CONFIG_MAX_BYTES + 1u) {
        size_t chunk = 0;
        result = storage__read(file, buffer + size, SSH_APP_CONFIG_MAX_BYTES + 1u - size, &chunk);
        if (result != BRUCE_OK || chunk == 0) break;
        size += chunk;
    }
    storage__close(file);
    if (result != BRUCE_OK || size > SSH_APP_CONFIG_MAX_BYTES) {
        ssh_app__buffer_free(buffer_data, buffer_external);
        return result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT;
    }
    buffer[size] = '\0';

    bool active = true;
    char *line_save = NULL;
    for (char *line = strtok_r(buffer, "\n", &line_save); line != NULL;
         line = strtok_r(NULL, "\n", &line_save)) {
        char *comment = strchr(line, '#');
        if (comment != NULL) *comment = '\0';
        line = ssh_app__trim(line);
        if (*line == '\0') continue;
        char *value = line;
        while (*value != '\0' && !isspace((unsigned char)*value) && *value != '=') ++value;
        if (*value == '\0') continue;
        *value++ = '\0';
        while (isspace((unsigned char)*value) || *value == '=') ++value;
        value = ssh_app__trim(value);
        if (strcasecmp(line, "Host") == 0) {
            active = ssh_app__host_list_matches(value, alias);
            continue;
        }
        if (!active || *value == '\0') continue;
        if (strcasecmp(line, "HostName") == 0)
            ssh_app__copy_config_value(
                config->hostname, sizeof(config->hostname), value, &config->hostname_set
            );
        else if (strcasecmp(line, "User") == 0)
            ssh_app__copy_config_value(
                config->username, sizeof(config->username), value, &config->username_set
            );
        else if (strcasecmp(line, "IdentityFile") == 0)
            ssh_app__copy_config_value(
                config->identity, sizeof(config->identity), value, &config->identity_set
            );
        else if (strcasecmp(line, "Port") == 0 && !config->port_set)
            config->port_set = ssh_app__parse_port(value, &config->port);
    }
    ssh_app__buffer_free(buffer_data, buffer_external);
    return BRUCE_OK;
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
    bool legacy = false;
    if (result == BRUCE_ERR_NOT_FOUND) {
        result = storage__open(SSH_APP_LEGACY_KNOWN_HOSTS_PATH, BRUCE_STORAGE_OPEN_READ, &file);
        legacy = result == BRUCE_OK;
    }
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
    if (legacy) {
        (void)storage__mkdir(SSH_APP_DIRECTORY);
        (void)storage__rename(SSH_APP_LEGACY_KNOWN_HOSTS_PATH, SSH_APP_KNOWN_HOSTS_PATH);
    }
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
static bruce_result_t
ssh_app__store_known_fingerprint(const char *key, const uint8_t fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]) {
    void *existing_data = NULL;
    void *rebuilt_data = NULL;
    bool existing_external = false;
    bool rebuilt_external = false;
    bruce_result_t alloc_result =
        ssh_app__buffer_alloc(&existing_data, &existing_external, SSH_APP_KNOWN_HOSTS_MAX_BYTES);
    if (alloc_result == BRUCE_OK)
        alloc_result =
            ssh_app__buffer_alloc(&rebuilt_data, &rebuilt_external, SSH_APP_KNOWN_HOSTS_MAX_BYTES);
    if (alloc_result != BRUCE_OK) {
        if (existing_data != NULL) ssh_app__buffer_free(existing_data, existing_external);
        return BRUCE_ERR_NO_MEMORY;
    }
    char *existing = existing_data;
    char *rebuilt = rebuilt_data;

    size_t existing_size = 0;
    bruce_result_t result =
        ssh_app__read_known_hosts(existing, SSH_APP_KNOWN_HOSTS_MAX_BYTES, &existing_size);
    if (result != BRUCE_OK) {
        ssh_app__buffer_free(existing_data, existing_external);
        ssh_app__buffer_free(rebuilt_data, rebuilt_external);
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
        bool is_match = space != NULL && (size_t)((const char *)space - line) == key_len &&
                        memcmp(line, key, key_len) == 0;
        if (!is_match && line_len > 0 && rebuilt_size + line_len + 1 <= SSH_APP_KNOWN_HOSTS_MAX_BYTES) {
            memcpy(rebuilt + rebuilt_size, line, line_len);
            rebuilt_size += line_len;
            rebuilt[rebuilt_size++] = '\n';
        }
        pos += line_len + 1;
    }
    ssh_app__buffer_free(existing_data, existing_external);

    char hex[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    ssh_app__hex_encode(fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE, hex);
    char new_line[SSH_APP_KEY_MAX + sizeof(hex) + 2];
    int new_line_len = snprintf(new_line, sizeof(new_line), "%s %s\n", key, hex);
    if (new_line_len > 0 && rebuilt_size + (size_t)new_line_len <= SSH_APP_KNOWN_HOSTS_MAX_BYTES) {
        memcpy(rebuilt + rebuilt_size, new_line, (size_t)new_line_len);
        rebuilt_size += (size_t)new_line_len;
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    result = storage__mkdir(SSH_APP_DIRECTORY);
    if (result != BRUCE_OK) {
        ssh_app__buffer_free(rebuilt_data, rebuilt_external);
        return result;
    }
    result = storage__open(
        SSH_APP_KNOWN_HOSTS_PATH,
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &file
    );
    if (result == BRUCE_OK) {
        size_t written = 0;
        result = storage__write(file, rebuilt, rebuilt_size, &written);
        storage__close(file);
    }
    ssh_app__buffer_free(rebuilt_data, rebuilt_external);
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

    void *known_hosts_data = NULL;
    bool known_hosts_external = false;
    if (ssh_app__buffer_alloc(
            &known_hosts_data, &known_hosts_external, SSH_APP_KNOWN_HOSTS_MAX_BYTES
        ) != BRUCE_OK)
        return BRUCE_ERR_NO_MEMORY;
    char *known_hosts = known_hosts_data;
    size_t known_hosts_size = 0;
    result = ssh_app__read_known_hosts(known_hosts, SSH_APP_KNOWN_HOSTS_MAX_BYTES, &known_hosts_size);
    if (result != BRUCE_OK) {
        ssh_app__buffer_free(known_hosts_data, known_hosts_external);
        return result;
    }
    uint8_t stored[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bool known = ssh_app__find_known_fingerprint(known_hosts, known_hosts_size, key, stored);
    ssh_app__buffer_free(known_hosts_data, known_hosts_external);

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
            key,
            hex
        );
        choices[0].label = "Trust new key & continue";
        choice_result = dialog__choice(
            "SSH host key changed", "The remote host key does not match the saved one.", choices, 2, &selected
        );
    } else {
        stdio__printf(
            "The authenticity of host '%s' can't be established.\nSHA256 fingerprint: %s\n", key, hex
        );
        choice_result = dialog__choice(
            "Unknown SSH host key",
            "Verify this fingerprint out-of-band before trusting it.",
            choices,
            2,
            &selected
        );
    }
    if (choice_result != BRUCE_OK || strcmp(choices[selected].value, "trust") != 0) return BRUCE_ERR_PERMISSION;

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

static bruce_result_t
ssh_app__session(bruce_ssh_id_t session, uint32_t tty_generation, bool *out_local_exit) {
    *out_local_exit = false;
    char received[SSH_APP_BUFFER_SIZE + 1];
    for (;;) {
        size_t received_size = 0;
        bruce_result_t result =
            ssh__read(session, false, received, SSH_APP_BUFFER_SIZE, SSH_APP_IO_TIMEOUT_MS, &received_size);
        if (result == BRUCE_OK) {
            if (received_size == 0) return BRUCE_OK;
            received[received_size] = '\0';
            (void)stdio__write(received, received_size);
        } else if (result != BRUCE_ERR_TIMEOUT) {
            return result;
        }

        /* Forwards local terminal resizes to the remote pty so full-screen
         * programs (htop, less, tmux, ...) re-layout immediately, mirroring
         * how a real ssh client reacts to SIGWINCH. tty__get_size()'s
         * generation only changes when the owning session's size actually
         * changed, so this is a cheap comparison on every idle poll rather
         * than an unconditional resize call. */
        bruce_tty_size_t local_size;
        if (tty__isatty() && tty__get_size(&local_size) == BRUCE_OK &&
            local_size.generation != tty_generation) {
            tty_generation = local_size.generation;
            (void)ssh__resize_pty(session, local_size.columns, local_size.rows, 2000);
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
 * failures on PSRAM-less boards. */
static void ssh_app__log_heap_state(const char *label) {
    bruce_memory_stats_t stats;
    if (memory__get_stats(&stats) != BRUCE_OK) return;
    stdio__printf(
        "[ssh mem] %s: internal free=%u largest=%u (total=%u)",
        label,
        (unsigned)stats.internal_free,
        (unsigned)stats.internal_largest_block,
        (unsigned)stats.internal_total
    );
    if (stats.psram_total > 0) {
        stdio__printf(
            ", psram free=%u largest=%u (total=%u)",
            (unsigned)stats.psram_free,
            (unsigned)stats.psram_largest_block,
            (unsigned)stats.psram_total
        );
    }
    stdio__printf("\n");
}

static bruce_result_t
ssh_app__read_private_key(const char *path, char *buffer, size_t capacity, size_t *out_size) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    size_t total = 0;
    while (total < capacity) {
        size_t read_size = 0;
        result = storage__read(file, buffer + total, capacity - total, &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        total += read_size;
    }
    storage__close(file);
    if (result != BRUCE_OK) return result;
    if (total == capacity) return BRUCE_ERR_RESOURCE_LIMIT;
    *out_size = total;
    return BRUCE_OK;
}

static bruce_result_t ssh_app__write_file(const char *path, const void *data, size_t size) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
    );
    if (result != BRUCE_OK) return result;
    size_t total = 0;
    while (total < size) {
        size_t written = 0;
        result = storage__write(file, (const char *)data + total, size - total, &written);
        if (result != BRUCE_OK || written == 0) break;
        total += written;
    }
    storage__close(file);
    return result == BRUCE_OK && total == size ? BRUCE_OK : result == BRUCE_OK ? BRUCE_ERR_IO : result;
}

static bool ssh_app__file_exists(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return false;
    storage__close(file);
    return true;
}

static const char *ssh_app__default_identity(void) {
    if (ssh_app__file_exists(SSH_APP_DEFAULT_ECDSA_IDENTITY_PATH)) {
        return SSH_APP_DEFAULT_ECDSA_IDENTITY_PATH;
    }
    if (ssh_app__file_exists(SSH_APP_DEFAULT_ED25519_IDENTITY_PATH)) {
        return SSH_APP_DEFAULT_ED25519_IDENTITY_PATH;
    }
    return "";
}

static bruce_result_t ssh_app__client(
    const char *host, uint16_t port, const char *username, const char *password, const char *identity
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

    char password_buffer[SSH_APP_PASSWORD_MAX] = {0};
    if (identity != NULL && identity[0] != '\0') {
        char private_key[BRUCE_SSH_PRIVATE_KEY_MAX_SIZE + 1u];
        size_t private_key_size = 0;
        result = ssh_app__read_private_key(identity, private_key, sizeof(private_key), &private_key_size);
        if (result == BRUCE_OK)
            result = ssh__authenticate_key(session, username, private_key, private_key_size, 10000);
        memset(private_key, 0, sizeof(private_key));
    } else {
        const char *effective_password = password;
        if (password == NULL || password[0] == '\0') {
            char prompt[sizeof("Password for @") + SSH_APP_USERNAME_MAX + SSH_APP_HOST_MAX];
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
    }
    if (result != BRUCE_OK) {
        if (identity != NULL && identity[0] != '\0')
            stdio__printf(
                result == BRUCE_ERR_INVALID_ARGUMENT || result == BRUCE_ERR_UNSUPPORTED
                    ? "SSH client: %s is not a valid unencrypted ECDSA P-256 or OpenSSH Ed25519 key (%d)\n"
                    : "SSH client: key authentication with %s failed (%d)\n",
                identity,
                result
            );
        else stdio__printf("SSH client: authentication failed (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }

    /* Opens the remote pty at the local screen's actual size (inherited from
     * whatever owns our routed stdio session -- typically terminal_app.c,
     * see its tty__set_size call) instead of a fixed guess, so full-screen
     * remote programs (htop, less, tmux, ...) render correctly from the
     * first frame. Falls back to the historical 80x24 default when no
     * session size is known yet (e.g. run from the physical serial console). */
    uint16_t pty_columns = SSH_APP_DEFAULT_COLUMNS;
    uint16_t pty_rows = SSH_APP_DEFAULT_ROWS;
    uint32_t pty_tty_generation = 0;
    bruce_tty_size_t local_size;
    if (tty__isatty() && tty__get_size(&local_size) == BRUCE_OK) {
        pty_columns = local_size.columns;
        pty_rows = local_size.rows;
        pty_tty_generation = local_size.generation;
    }
    result = ssh__open_shell(session, "xterm", pty_columns, pty_rows, 10000);
    if (result != BRUCE_OK) {
        stdio__printf("SSH client: failed to open shell (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }

    stdio__printf("Connected. Press Ctrl+] to close.\n");
    bool local_exit = false;
    result = ssh_app__session(session, pty_tty_generation, &local_exit);
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
        "shell. Use --identity with a key created by ssh-keygen. Press Ctrl+] to close."
    );
    ap_add_required_arg(root, "host", "Remote host name or address");
    ap_add_optional_arg(root, "port", "Remote SSH port (default 22 or value from ~/.ssh/config)");
    ap_add_optional_arg(root, "username", "Login username (or value from ~/.ssh/config)");
    ap_add_str_opt(root, "password", "");
    ap_set_opt_help(root, "password", "Password (prompted securely if omitted)");
    ap_add_str_opt(root, "identity", "");
    ap_set_opt_help(root, "identity", "Path to an ECDSA private key; disables password authentication");

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION) ap_print_help(root);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }

    uint16_t port = 22;
    const char *port_text = ap_get_arg(root, "port");
    if (port_text != NULL && !ssh_app__parse_port(port_text, &port)) {
        ap_print_help(root);
        ap_free(root);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char alias[SSH_APP_HOST_MAX];
    char host[SSH_APP_HOST_MAX];
    char username[SSH_APP_USERNAME_MAX];
    char password_copy[SSH_APP_PASSWORD_MAX];
    char identity_copy[SSH_APP_PATH_MAX];
    int alias_len = snprintf(alias, sizeof(alias), "%s", ap_get_arg(root, "host"));
    ssh_app__config_t config = {0};
    bruce_result_t config_result = alias_len > 0 && (size_t)alias_len < sizeof(alias)
                                       ? ssh_app__load_config(alias, &config)
                                       : BRUCE_ERR_INVALID_ARGUMENT;
    int host_len = snprintf(host, sizeof(host), "%s", config.hostname_set ? config.hostname : alias);
    const char *username_arg = ap_get_arg(root, "username");
    int username_len = snprintf(
        username,
        sizeof(username),
        "%s",
        username_arg != NULL  ? username_arg
        : config.username_set ? config.username
                              : ""
    );
    if (port_text == NULL && config.port_set) port = config.port;
    const char *password = ap_get_str_value(root, "password");
    snprintf(password_copy, sizeof(password_copy), "%s", password != NULL ? password : "");
    const char *identity = ap_get_str_value(root, "identity");
    bool password_supplied = ap_found(root, "password");
    bool identity_supplied = identity != NULL && identity[0] != '\0';
    const char *effective_identity = identity_supplied                           ? identity
                                     : !password_supplied && config.identity_set ? config.identity
                                     : !password_supplied                        ? ssh_app__default_identity()
                                                                                 : "";
    if (strncmp(effective_identity, "~/", 2) == 0) ++effective_identity;
    int identity_len = snprintf(identity_copy, sizeof(identity_copy), "%s", effective_identity);
    bool conflicting_auth = password_supplied && identity_supplied;
    ap_free(root);

    /* Each of these used to fall through to one shared `return
     * BRUCE_ERR_INVALID_ARGUMENT` with no message at all -- every other
     * failure path in this file (Wi-Fi down, connection failed, host key
     * rejected, auth failed, ...) prints a "SSH client: ..." line first, so
     * silently landing here just looked like the command did nothing. The
     * likeliest way to hit this in practice: `ssh <alias>` for an alias with
     * no --username on the command line and no matching `User` line under
     * `Host <alias>` in ~/.ssh/config, which leaves username_len == 0. */
    if (config_result != BRUCE_OK) {
        stdio__printf("SSH client: failed to read %s (%d)\n", SSH_APP_CONFIG_PATH, config_result);
        memset(password_copy, 0, sizeof(password_copy));
        return config_result;
    }
    if (alias_len <= 0 || (size_t)alias_len >= sizeof(alias)) {
        stdio__printf("SSH client: host name is empty or too long (max %zu characters)\n", sizeof(alias) - 1);
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (host_len < 0 || (size_t)host_len >= sizeof(host)) {
        stdio__printf("SSH client: resolved host name is too long (max %zu characters)\n", sizeof(host) - 1);
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (username_len <= 0) {
        stdio__printf(
            "SSH client: no username for '%s' -- pass --username, or add 'User <name>' under 'Host %s' in %s\n",
            alias, alias, SSH_APP_CONFIG_PATH
        );
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if ((size_t)username_len >= sizeof(username)) {
        stdio__printf("SSH client: username is too long (max %zu characters)\n", sizeof(username) - 1);
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (identity_len < 0 || (size_t)identity_len >= sizeof(identity_copy)) {
        stdio__printf(
            "SSH client: identity file path is too long (max %zu characters)\n", sizeof(identity_copy) - 1
        );
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (identity_copy[0] != '\0' && identity_copy[0] != '/') {
        stdio__printf("SSH client: identity file path '%s' must be absolute (start with '/')\n", identity_copy);
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (conflicting_auth) {
        stdio__printf("SSH client: --password and --identity cannot both be given\n");
        memset(password_copy, 0, sizeof(password_copy));
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    int result = ssh_app__client(host, port, username, password_copy, identity_copy);
    memset(password_copy, 0, sizeof(password_copy));
    return result;
}

int ssh_keygen_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(
        parser,
        "Generate an ECDSA P-256 or Ed25519 SSH keypair. Writes the private key to --file and the OpenSSH "
        "public key to <file>.pub."
    );
    ap_add_str_opt(parser, "file", "");
    ap_set_opt_help(parser, "file", "Private-key output path (default depends on --type)");
    ap_add_str_opt(parser, "type", "ecdsa");
    ap_set_opt_help(parser, "type", "Key type: ecdsa or ed25519 (default ecdsa)");
    ap_add_str_opt(parser, "comment", "bruce");
    ap_set_opt_help(parser, "comment", "Comment appended to the public key");
    ap_add_flag(parser, "force");
    ap_set_opt_help(parser, "force", "Overwrite existing key files");
    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION) ap_print_help(parser);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(parser);
        return result;
    }

    char path[SSH_APP_PATH_MAX];
    char public_path[SSH_APP_PATH_MAX];
    char comment[SSH_APP_COMMENT_MAX];
    const char *path_arg = ap_get_str_value(parser, "file");
    const char *type_arg = ap_get_str_value(parser, "type");
    const char *comment_arg = ap_get_str_value(parser, "comment");
    bool force = ap_found(parser, "force");
    bruce_ssh_key_type_t key_type = type_arg != NULL && strcasecmp(type_arg, "ed25519") == 0
                                        ? BRUCE_SSH_KEY_ED25519
                                        : BRUCE_SSH_KEY_ECDSA_P256;
    bool valid_type =
        type_arg != NULL && (strcasecmp(type_arg, "ecdsa") == 0 || strcasecmp(type_arg, "ed25519") == 0);
    const char *effective_path = path_arg != NULL && path_arg[0] != '\0' ? path_arg
                                 : key_type == BRUCE_SSH_KEY_ED25519 ? SSH_APP_DEFAULT_ED25519_IDENTITY_PATH
                                                                     : SSH_APP_DEFAULT_ECDSA_IDENTITY_PATH;
    int path_len = snprintf(path, sizeof(path), "%s", effective_path);
    int public_path_len = snprintf(public_path, sizeof(public_path), "%s.pub", path);
    int comment_len = snprintf(comment, sizeof(comment), "%s", comment_arg != NULL ? comment_arg : "");
    ap_free(parser);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path) || public_path_len <= 0 ||
        (size_t)public_path_len >= sizeof(public_path) || comment_len < 0 ||
        (size_t)comment_len >= sizeof(comment) || !valid_type)
        return BRUCE_ERR_INVALID_ARGUMENT;
    if (!force && (ssh_app__file_exists(path) || ssh_app__file_exists(public_path))) {
        stdio__printf("ssh-keygen: output exists; use --force to overwrite\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    if (strncmp(path, SSH_APP_DIRECTORY "/", sizeof(SSH_APP_DIRECTORY)) == 0) {
        bruce_result_t mkdir_result = storage__mkdir(SSH_APP_DIRECTORY);
        if (mkdir_result != BRUCE_OK) return mkdir_result;
    }

    char private_key[BRUCE_SSH_PRIVATE_KEY_MAX_SIZE];
    char public_key[BRUCE_SSH_PUBLIC_KEY_MAX_SIZE];
    size_t private_size = 0;
    size_t public_size = 0;
    bruce_result_t result = ssh__generate_keypair_ex(
        key_type,
        private_key,
        sizeof(private_key),
        &private_size,
        public_key,
        sizeof(public_key),
        &public_size
    );
    if (result == BRUCE_OK && comment[0] != '\0') {
        size_t required = public_size + 1u + strlen(comment) + 1u;
        if (required > sizeof(public_key)) result = BRUCE_ERR_RESOURCE_LIMIT;
        else {
            public_key[public_size++] = ' ';
            memcpy(public_key + public_size, comment, strlen(comment));
            public_size += strlen(comment);
            public_key[public_size++] = '\n';
        }
    } else if (result == BRUCE_OK) {
        public_key[public_size++] = '\n';
    }
    if (result == BRUCE_OK) result = ssh_app__write_file(path, private_key, private_size);
    if (result == BRUCE_OK) result = ssh_app__write_file(public_path, public_key, public_size);
    memset(private_key, 0, sizeof(private_key));
    if (result != BRUCE_OK) {
        stdio__printf("ssh-keygen: failed (%d)\n", result);
        return result;
    }
    stdio__printf("Private key: %s\nPublic key: %s\n", path, public_path);
    return BRUCE_OK;
}
