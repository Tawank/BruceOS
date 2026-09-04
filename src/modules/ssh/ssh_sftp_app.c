#include "ssh_sftp_app.h"
#include "ssh_sftp_internal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core_sdk/dialog.h"
#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/ssh.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/wifi.h"

/**
 * @brief SFTP client: the "sftp" provider for filemanager's "/Network" folder.
 *
 * Three ways this gets invoked, all sharing the connect/auth/browse code
 * below:
 *   - `sftp list --autodiscover`: headless, prints one
 *     "<display name>\t<location>" line per SSH host alias found in
 *     "/.ssh/config", plus a "New connection..." line with an empty
 *     location. filemanager runs this to populate "/Network" -- see
 *     filemanager_app.c's "Network folder" section.
 *   - `sftp <path>`: extensions.conf dispatches a ".sftp" file opened from
 *     filemanager here (".sftp" -> program "sftp", see extensions.json).
 *     The file's content is the location: an empty file (the "New
 *     connection..." entry above) prompts for a fresh host/username/auth;
 *     any other content is resolved as a host alias the same way `ssh
 *     <alias>` resolves one, through "/.ssh/config".
 *   - `sftp` with no arguments: same as opening an empty location -- prompts
 *     for a new connection directly.
 *
 * Deliberately independent of ssh_app.c (which owns the interactive shell
 * client, "ssh"/"ssh-keygen"): an SSH channel is fixed at connect time to
 * carry either a shell or the SFTP subsystem (see the SFTP group comment in
 * core_sdk/ssh.h), so this needs its own authenticate/host-key-check code
 * rather than reusing ssh_app.c's -- which also means ssh_app.c never has to
 * change for this feature to exist. It shares ssh_app.c's known_hosts store
 * ("/.ssh/known_hosts") and "/.ssh/config" so both apps trust and resolve
 * hosts the same way, and read-only: there is no upload/rename/delete here,
 * matching core_sdk/ssh.h's SFTP group.
 */

#define SFTP_APP_PASSWORD_MAX 128u
#define SFTP_APP_KEY_MAX 96u
#define SFTP_APP_DIRECTORY "/.ssh"
#define SFTP_APP_KNOWN_HOSTS_PATH "/.ssh/known_hosts"
#define SFTP_APP_CONFIG_PATH "/.ssh/config"
#define SFTP_APP_DEFAULT_ECDSA_IDENTITY_PATH "/.ssh/id_ecdsa"
#define SFTP_APP_DEFAULT_ED25519_IDENTITY_PATH "/.ssh/id_ed25519"
#define SFTP_APP_KNOWN_HOSTS_MAX_BYTES 4096u
#define SFTP_APP_CONFIG_MAX_BYTES 4096u
#define SFTP_APP_ENTRY_MAX 64u
#define SFTP_APP_REMOTE_PATH_MAX 512u
#define SFTP_APP_VIEW_MAX 8192u
#define SFTP_APP_DOWNLOAD_CHUNK 4096u
#define SFTP_APP_DOWNLOAD_DIR "/Downloads"

/* SFTP_APP_HOST_MAX/USERNAME_MAX/PATH_MAX and sftp_app__config_t itself now
 * live in ssh_sftp_internal.h, alongside the pure parsing/matching helpers
 * below -- selftest unit-tests those directly (see its header comment). */

/**
 * @name Config/known_hosts plumbing
 *
 * Deliberate near-duplicates of ssh_app.c's equivalents: modules can't share
 * static helpers across files (and each other's headers only expose their
 * *_app_main() entry point), and this is the isolation the module/core.h
 * boundary rule already accepts elsewhere in this codebase. Behavior stays
 * identical on purpose -- same known_hosts format, same "/.ssh/config"
 * parsing rules -- so trust and host resolution are consistent between
 * `ssh` and `sftp`.
 * @{
 */

bool sftp_app__parse_port(const char *text, uint16_t *out_port) {
    if (text == NULL || out_port == NULL || text[0] == '\0') return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT16_MAX) return false;
    *out_port = (uint16_t)value;
    return true;
}

bool sftp_app__host_pattern_matches(const char *pattern, const char *host) {
    while (*pattern != '\0') {
        if (*pattern == '*') {
            while (*pattern == '*') ++pattern;
            if (*pattern == '\0') return true;
            while (*host != '\0') {
                if (sftp_app__host_pattern_matches(pattern, host)) return true;
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

bool sftp_app__host_list_matches(char *patterns, const char *host) {
    bool matched = false;
    char *save = NULL;
    for (char *pattern = strtok_r(patterns, " \t", &save); pattern != NULL;
         pattern = strtok_r(NULL, " \t", &save)) {
        bool negated = pattern[0] == '!';
        if (negated) ++pattern;
        if (sftp_app__host_pattern_matches(pattern, host)) {
            if (negated) return false;
            matched = true;
        }
    }
    return matched;
}

bool sftp_app__is_literal_host_token(const char *token) {
    if (token[0] == '\0' || token[0] == '!') return false;
    for (const char *c = token; *c != '\0'; ++c) {
        if (*c == '*' || *c == '?') return false;
    }
    return true;
}

char *sftp_app__trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

void sftp_app__copy_config_value(char *out, size_t capacity, const char *value, bool *was_set) {
    if (*was_set) return;
    int length = snprintf(out, capacity, "%s", value);
    if (length >= 0 && (size_t)length < capacity) *was_set = true;
}

static bruce_result_t sftp_app__buffer_alloc(void **out_data, bool *out_external, size_t size) {
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

static void sftp_app__buffer_free(void *data, bool external) {
    if (external) (void)memory__external_free(data);
    else memory__free(data);
}

static bool sftp_app__file_exists(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return false;
    storage__close(file);
    return true;
}

static const char *sftp_app__default_identity(void) {
    if (sftp_app__file_exists(SFTP_APP_DEFAULT_ECDSA_IDENTITY_PATH)) return SFTP_APP_DEFAULT_ECDSA_IDENTITY_PATH;
    if (sftp_app__file_exists(SFTP_APP_DEFAULT_ED25519_IDENTITY_PATH))
        return SFTP_APP_DEFAULT_ED25519_IDENTITY_PATH;
    return "";
}

static bruce_result_t
sftp_app__read_private_key(const char *path, char *buffer, size_t capacity, size_t *out_size) {
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

/* Loads whichever "/.ssh/config" fields apply to `alias`, same resolution
 * rule as ssh_app.c's equivalent: the first matching value for each
 * directive under any matching "Host" block wins. Missing entirely (no
 * "/.ssh/config" at all) isn't an error -- every field is simply left unset,
 * same as ssh_app.c treats it. */
static bruce_result_t sftp_app__load_config(const char *alias, sftp_app__config_t *config) {
    void *buffer_data = NULL;
    bool buffer_external = false;
    if (sftp_app__buffer_alloc(&buffer_data, &buffer_external, SFTP_APP_CONFIG_MAX_BYTES + 1u) != BRUCE_OK)
        return BRUCE_ERR_NO_MEMORY;
    char *buffer = buffer_data;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(SFTP_APP_CONFIG_PATH, BRUCE_STORAGE_OPEN_READ, &file);
    if (result == BRUCE_ERR_NOT_FOUND) {
        sftp_app__buffer_free(buffer_data, buffer_external);
        return BRUCE_OK;
    }
    if (result != BRUCE_OK) {
        sftp_app__buffer_free(buffer_data, buffer_external);
        return result;
    }
    size_t size = 0;
    while (size < SFTP_APP_CONFIG_MAX_BYTES + 1u) {
        size_t chunk = 0;
        result = storage__read(file, buffer + size, SFTP_APP_CONFIG_MAX_BYTES + 1u - size, &chunk);
        if (result != BRUCE_OK || chunk == 0) break;
        size += chunk;
    }
    storage__close(file);
    if (result != BRUCE_OK || size > SFTP_APP_CONFIG_MAX_BYTES) {
        sftp_app__buffer_free(buffer_data, buffer_external);
        return result != BRUCE_OK ? result : BRUCE_ERR_RESOURCE_LIMIT;
    }
    buffer[size] = '\0';

    bool active = true;
    char *line_save = NULL;
    for (char *line = strtok_r(buffer, "\n", &line_save); line != NULL;
         line = strtok_r(NULL, "\n", &line_save)) {
        char *comment = strchr(line, '#');
        if (comment != NULL) *comment = '\0';
        line = sftp_app__trim(line);
        if (*line == '\0') continue;
        char *value = line;
        while (*value != '\0' && !isspace((unsigned char)*value) && *value != '=') ++value;
        if (*value == '\0') continue;
        *value++ = '\0';
        while (isspace((unsigned char)*value) || *value == '=') ++value;
        value = sftp_app__trim(value);
        if (strcasecmp(line, "Host") == 0) {
            active = sftp_app__host_list_matches(value, alias);
            continue;
        }
        if (!active || *value == '\0') continue;
        if (strcasecmp(line, "HostName") == 0)
            sftp_app__copy_config_value(config->hostname, sizeof(config->hostname), value, &config->hostname_set);
        else if (strcasecmp(line, "User") == 0)
            sftp_app__copy_config_value(config->username, sizeof(config->username), value, &config->username_set);
        else if (strcasecmp(line, "IdentityFile") == 0)
            sftp_app__copy_config_value(config->identity, sizeof(config->identity), value, &config->identity_set);
        else if (strcasecmp(line, "Port") == 0 && !config->port_set)
            config->port_set = sftp_app__parse_port(value, &config->port);
    }
    sftp_app__buffer_free(buffer_data, buffer_external);
    return BRUCE_OK;
}

void sftp_app__hex_encode(const uint8_t *bytes, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0Fu];
    }
    out[size * 2] = '\0';
}

int sftp_app__hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool sftp_app__hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_size) {
    if (hex_len != out_size * 2) return false;
    for (size_t i = 0; i < out_size; ++i) {
        int high = sftp_app__hex_value(hex[i * 2]);
        int low = sftp_app__hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

/* known_hosts entries are "<host>|<port> <64 hex chars>\n", same format and
 * same file ssh_app.c's client uses -- see its equivalent for why '|'
 * rather than ':' separates host from port (IPv6 literals). */
static bruce_result_t sftp_app__read_known_hosts(char *buffer, size_t capacity, size_t *out_size) {
    *out_size = 0;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(SFTP_APP_KNOWN_HOSTS_PATH, BRUCE_STORAGE_OPEN_READ, &file);
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

bool sftp_app__find_known_fingerprint(
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
                sftp_app__hex_decode((const char *)space + 1, hex_len, out_fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE))
                return true;
        }
        pos += line_len + 1;
    }
    return false;
}

static bruce_result_t
sftp_app__store_known_fingerprint(const char *key, const uint8_t fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]) {
    void *existing_data = NULL;
    void *rebuilt_data = NULL;
    bool existing_external = false;
    bool rebuilt_external = false;
    bruce_result_t alloc_result = sftp_app__buffer_alloc(&existing_data, &existing_external, SFTP_APP_KNOWN_HOSTS_MAX_BYTES);
    if (alloc_result == BRUCE_OK)
        alloc_result = sftp_app__buffer_alloc(&rebuilt_data, &rebuilt_external, SFTP_APP_KNOWN_HOSTS_MAX_BYTES);
    if (alloc_result != BRUCE_OK) {
        if (existing_data != NULL) sftp_app__buffer_free(existing_data, existing_external);
        return BRUCE_ERR_NO_MEMORY;
    }
    char *existing = existing_data;
    char *rebuilt = rebuilt_data;

    size_t existing_size = 0;
    bruce_result_t result = sftp_app__read_known_hosts(existing, SFTP_APP_KNOWN_HOSTS_MAX_BYTES, &existing_size);
    if (result != BRUCE_OK) {
        sftp_app__buffer_free(existing_data, existing_external);
        sftp_app__buffer_free(rebuilt_data, rebuilt_external);
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
        if (!is_match && line_len > 0 && rebuilt_size + line_len + 1 <= SFTP_APP_KNOWN_HOSTS_MAX_BYTES) {
            memcpy(rebuilt + rebuilt_size, line, line_len);
            rebuilt_size += line_len;
            rebuilt[rebuilt_size++] = '\n';
        }
        pos += line_len + 1;
    }
    sftp_app__buffer_free(existing_data, existing_external);

    char hex[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    sftp_app__hex_encode(fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE, hex);
    char new_line[SFTP_APP_KEY_MAX + sizeof(hex) + 2];
    int new_line_len = snprintf(new_line, sizeof(new_line), "%s %s\n", key, hex);
    if (new_line_len > 0 && rebuilt_size + (size_t)new_line_len <= SFTP_APP_KNOWN_HOSTS_MAX_BYTES) {
        memcpy(rebuilt + rebuilt_size, new_line, (size_t)new_line_len);
        rebuilt_size += (size_t)new_line_len;
    }

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    result = storage__mkdir(SFTP_APP_DIRECTORY);
    if (result != BRUCE_OK) {
        sftp_app__buffer_free(rebuilt_data, rebuilt_external);
        return result;
    }
    result = storage__open(
        SFTP_APP_KNOWN_HOSTS_PATH, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &file
    );
    if (result == BRUCE_OK) {
        size_t written = 0;
        result = storage__write(file, rebuilt, rebuilt_size, &written);
        storage__close(file);
    }
    sftp_app__buffer_free(rebuilt_data, rebuilt_external);
    return result;
}

/* Same TOFU policy as ssh_app.c's ssh_app__verify_host_key(): a first-seen
 * key is shown for out-of-band verification and remembered; a later
 * mismatch is a loud warning, not a silent accept. */
static bruce_result_t sftp_app__verify_host_key(bruce_ssh_id_t session, const char *host, uint16_t port) {
    uint8_t fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bruce_result_t result = ssh__host_key_sha256(session, fingerprint);
    if (result != BRUCE_OK) return result;

    char hex[BRUCE_SSH_HOST_KEY_SHA256_SIZE * 2 + 1];
    sftp_app__hex_encode(fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE, hex);

    char key[SFTP_APP_KEY_MAX];
    snprintf(key, sizeof(key), "%s|%u", host, (unsigned int)port);

    void *known_hosts_data = NULL;
    bool known_hosts_external = false;
    if (sftp_app__buffer_alloc(&known_hosts_data, &known_hosts_external, SFTP_APP_KNOWN_HOSTS_MAX_BYTES) != BRUCE_OK)
        return BRUCE_ERR_NO_MEMORY;
    char *known_hosts = known_hosts_data;
    size_t known_hosts_size = 0;
    result = sftp_app__read_known_hosts(known_hosts, SFTP_APP_KNOWN_HOSTS_MAX_BYTES, &known_hosts_size);
    if (result != BRUCE_OK) {
        sftp_app__buffer_free(known_hosts_data, known_hosts_external);
        return result;
    }
    uint8_t stored[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    bool known = sftp_app__find_known_fingerprint(known_hosts, known_hosts_size, key, stored);
    sftp_app__buffer_free(known_hosts_data, known_hosts_external);

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
        choice_result = dialog__choice(
            "SFTP host key changed", "The remote host key does not match the saved one.", choices, 2, &selected
        );
    } else {
        stdio__printf("The authenticity of host '%s' can't be established.\nSHA256 fingerprint: %s\n", key, hex);
        choice_result = dialog__choice(
            "Unknown SSH host key", "Verify this fingerprint out-of-band before trusting it.", choices, 2, &selected
        );
    }
    if (choice_result != BRUCE_OK || strcmp(choices[selected].value, "trust") != 0) return BRUCE_ERR_PERMISSION;

    result = ssh__verify_host_key_sha256(session, fingerprint);
    if (result != BRUCE_OK) return result;
    return sftp_app__store_known_fingerprint(key, fingerprint);
}

/** @} */

/* Headless: prints one "<display name>\t<location>" line per location this
 * provider knows about, for filemanager to capture and materialize into
 * "/Network" (see filemanager_app.c's "Network folder" section). "New
 * connection..." always comes first with an empty location -- sftp_app__open()
 * below treats an empty location as "prompt for a new connection" rather
 * than something to resolve/connect to, so this is a plain file (like every
 * other discovered location), not a special case filemanager has to know
 * about. */
static void sftp_app__list_autodiscover(void) {
    stdio__printf("New connection...\t\n");

    void *buffer_data = NULL;
    bool buffer_external = false;
    if (sftp_app__buffer_alloc(&buffer_data, &buffer_external, SFTP_APP_CONFIG_MAX_BYTES + 1u) != BRUCE_OK) return;
    char *buffer = buffer_data;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(SFTP_APP_CONFIG_PATH, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) {
        sftp_app__buffer_free(buffer_data, buffer_external);
        return;
    }
    size_t size = 0;
    for (;;) {
        size_t chunk = 0;
        if (storage__read(file, buffer + size, SFTP_APP_CONFIG_MAX_BYTES - size, &chunk) != BRUCE_OK ||
            chunk == 0)
            break;
        size += chunk;
        if (size >= SFTP_APP_CONFIG_MAX_BYTES) break;
    }
    storage__close(file);
    buffer[size] = '\0';

    char *line_save = NULL;
    for (char *line = strtok_r(buffer, "\n", &line_save); line != NULL; line = strtok_r(NULL, "\n", &line_save)) {
        char *comment = strchr(line, '#');
        if (comment != NULL) *comment = '\0';
        line = sftp_app__trim(line);
        char *value = line;
        while (*value != '\0' && !isspace((unsigned char)*value)) ++value;
        if (*value == '\0' || strcasecmp(line, "Host") != 0) continue;
        *value++ = '\0';
        value = sftp_app__trim(value);
        char *token_save = NULL;
        for (char *token = strtok_r(value, " \t", &token_save); token != NULL;
             token = strtok_r(NULL, " \t", &token_save)) {
            if (sftp_app__is_literal_host_token(token)) stdio__printf("%s\t%s\n", token, token);
        }
    }
    sftp_app__buffer_free(buffer_data, buffer_external);
}

/* Connects, verifies the host key, authenticates (key if one's configured or
 * found under the default names, else a password prompt), and completes the
 * SFTP handshake -- everything sftp_app__browse() needs before it can list a
 * directory. */
static bruce_result_t sftp_app__connect(
    const char *host, uint16_t port, const char *username, const char *identity, bruce_ssh_id_t *out_session
) {
    if (!wifi__is_connected()) {
        stdio__printf("SFTP client: Wi-Fi is not connected\n");
        return BRUCE_ERR_INVALID_STATE;
    }
    stdio__printf("Connecting to %s:%u...\n", host, (unsigned int)port);
    bruce_ssh_id_t session = BRUCE_SSH_ID_INVALID;
    bruce_result_t result = ssh__connect(host, port, 10000, &session);
    if (result != BRUCE_OK) {
        stdio__printf("SFTP client: connection failed (%d)\n", result);
        return result;
    }

    result = sftp_app__verify_host_key(session, host, port);
    if (result != BRUCE_OK) {
        stdio__printf("SFTP client: host key not verified, aborting (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }

    if (identity != NULL && identity[0] != '\0') {
        char private_key[BRUCE_SSH_PRIVATE_KEY_MAX_SIZE + 1u];
        size_t private_key_size = 0;
        result = sftp_app__read_private_key(identity, private_key, sizeof(private_key), &private_key_size);
        if (result == BRUCE_OK)
            result = ssh__sftp_authenticate_key(session, username, private_key, private_key_size, 10000);
        memset(private_key, 0, sizeof(private_key));
    } else {
        char password_buffer[SFTP_APP_PASSWORD_MAX] = {0};
        char prompt[sizeof("Password for @") + SFTP_APP_USERNAME_MAX + SFTP_APP_HOST_MAX];
        snprintf(prompt, sizeof(prompt), "Password for %s@%s", username, host);
        result = dialog__text_input("SFTP", prompt, NULL, true, password_buffer, sizeof(password_buffer));
        if (result != BRUCE_OK) {
            (void)ssh__close(session);
            return result;
        }
        result = ssh__sftp_authenticate_password(session, username, password_buffer, 10000);
        memset(password_buffer, 0, sizeof(password_buffer));
    }
    if (result != BRUCE_OK) {
        stdio__printf("SFTP client: authentication failed (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }

    result = ssh__sftp_open(session, 10000);
    if (result != BRUCE_OK) {
        stdio__printf("SFTP client: protocol handshake failed (%d)\n", result);
        (void)ssh__close(session);
        return result;
    }
    *out_session = session;
    return BRUCE_OK;
}

static void sftp_app__ascend(char *remote_dir) {
    char *slash = strrchr(remote_dir, '/');
    if (slash == NULL) {
        remote_dir[0] = '.';
        remote_dir[1] = '\0';
    } else {
        *slash = '\0';
    }
}

static bruce_result_t sftp_app__descend(char *remote_dir, size_t capacity, const char *name) {
    char joined[SFTP_APP_REMOTE_PATH_MAX];
    int written = strcmp(remote_dir, ".") == 0 ? snprintf(joined, sizeof(joined), "./%s", name)
                                                : snprintf(joined, sizeof(joined), "%s/%s", remote_dir, name);
    if (written < 0 || (size_t)written >= sizeof(joined) || (size_t)written >= capacity)
        return BRUCE_ERR_RESOURCE_LIMIT;
    memcpy(remote_dir, joined, (size_t)written + 1u);
    return BRUCE_OK;
}

static bruce_result_t sftp_app__build_download_path(const char *name, char *out_path, size_t out_size) {
    bruce_result_t mkdir_result = storage__mkdir(SFTP_APP_DOWNLOAD_DIR);
    if (mkdir_result != BRUCE_OK) return mkdir_result;
    for (int suffix = 0; suffix < 1000; ++suffix) {
        int written = suffix == 0 ? snprintf(out_path, out_size, "%s/%s", SFTP_APP_DOWNLOAD_DIR, name)
                                   : snprintf(out_path, out_size, "%s/%s_%d", SFTP_APP_DOWNLOAD_DIR, name, suffix);
        if (written < 0 || (size_t)written >= out_size) return BRUCE_ERR_RESOURCE_LIMIT;
        bool exists = false;
        bruce_result_t result = storage__exists(out_path, &exists);
        if (result != BRUCE_OK) return result;
        if (!exists) return BRUCE_OK;
    }
    return BRUCE_ERR_RESOURCE_LIMIT;
}

static bruce_result_t sftp_app__download(bruce_ssh_id_t session, const char *remote_path, const char *name) {
    char local_path[BRUCE_STORAGE_PATH_MAX];
    bruce_result_t result = sftp_app__build_download_path(name, local_path, sizeof(local_path));
    if (result != BRUCE_OK) {
        dialog__message(BRUCE_DIALOG_ERROR, "Download", "Could not pick a destination file name.");
        return result;
    }

    bruce_ssh_sftp_file_t remote_file;
    result = ssh__sftp_open_file(session, remote_path, &remote_file, 10000);
    if (result != BRUCE_OK) {
        dialog__message(BRUCE_DIALOG_ERROR, "Download", "Could not open the remote file.");
        return result;
    }

    bruce_file_id_t local_file = BRUCE_FILE_ID_INVALID;
    result = storage__open(
        local_path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &local_file
    );
    if (result != BRUCE_OK) {
        (void)ssh__sftp_close_file(session, &remote_file, 10000);
        dialog__message(BRUCE_DIALOG_ERROR, "Download", "Could not create the local file.");
        return result;
    }

    char *chunk = memory__malloc(SFTP_APP_DOWNLOAD_CHUNK);
    if (chunk == NULL) {
        storage__close(local_file);
        (void)ssh__sftp_close_file(session, &remote_file, 10000);
        return BRUCE_ERR_NO_MEMORY;
    }

    stdio__printf("Downloading to %s...\n", local_path);
    uint64_t offset = 0;
    for (;;) {
        size_t read_size = 0;
        result = ssh__sftp_read_file(session, &remote_file, chunk, SFTP_APP_DOWNLOAD_CHUNK, offset, &read_size, 15000);
        if (result != BRUCE_OK || read_size == 0) break;
        size_t written_size = 0;
        result = storage__write(local_file, chunk, read_size, &written_size);
        if (result != BRUCE_OK || written_size != read_size) {
            result = result == BRUCE_OK ? BRUCE_ERR_IO : result;
            break;
        }
        offset += read_size;
    }
    memory__free(chunk);
    storage__close(local_file);
    (void)ssh__sftp_close_file(session, &remote_file, 10000);

    if (result != BRUCE_OK) {
        stdio__printf("Download failed (%d)\n", result);
        dialog__message(BRUCE_DIALOG_ERROR, "Download", "The transfer failed partway through.");
        return result;
    }
    char message[BRUCE_STORAGE_PATH_MAX + 16];
    snprintf(message, sizeof(message), "Saved to %s", local_path);
    dialog__message(BRUCE_DIALOG_SUCCESS, "Download complete", message);
    return BRUCE_OK;
}

static bruce_result_t sftp_app__view(bruce_ssh_id_t session, const char *remote_path, const char *name) {
    bruce_ssh_sftp_file_t remote_file;
    bruce_result_t result = ssh__sftp_open_file(session, remote_path, &remote_file, 10000);
    if (result != BRUCE_OK) {
        dialog__message(BRUCE_DIALOG_ERROR, "View", "Could not open the remote file.");
        return result;
    }

    char *text = memory__malloc(SFTP_APP_VIEW_MAX + 1u);
    if (text == NULL) {
        (void)ssh__sftp_close_file(session, &remote_file, 10000);
        return BRUCE_ERR_NO_MEMORY;
    }
    size_t total = 0;
    while (total < SFTP_APP_VIEW_MAX) {
        size_t read_size = 0;
        result = ssh__sftp_read_file(
            session, &remote_file, text + total, SFTP_APP_VIEW_MAX - total, total, &read_size, 15000
        );
        if (result != BRUCE_OK || read_size == 0) break;
        total += read_size;
    }
    (void)ssh__sftp_close_file(session, &remote_file, 10000);
    if (result != BRUCE_OK) {
        memory__free(text);
        dialog__message(BRUCE_DIALOG_ERROR, "View", "The transfer failed partway through.");
        return result;
    }
    for (size_t i = 0; i < total; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\0' || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) text[i] = '.';
    }
    text[total] = '\0';

    bruce_viewer_id_t viewer = BRUCE_VIEWER_ID_INVALID;
    result = dialog__create_text_viewer(name, text, &viewer);
    memory__free(text);
    if (result != BRUCE_OK) return result;
    bruce_dialog_choice_t dismiss[] = {
        {.label = "Back", .value = "back"}
    };
    size_t selected = 0;
    (void)dialog__choice(NULL, NULL, dismiss, 1, &selected);
    return dialog__viewer_close(viewer);
}

/* Interactive directory browser: lists the current remote directory, lets
 * the user descend into subdirectories or go back up, and offers
 * View/Download on a chosen file. Starts at "." (the login/home directory)
 * and never goes above it -- a v1 scope limitation, not a protocol one. */
static void sftp_app__browse(bruce_ssh_id_t session) {
    char remote_dir[SFTP_APP_REMOTE_PATH_MAX] = ".";
    bruce_ssh_sftp_entry_t *entries = memory__malloc(SFTP_APP_ENTRY_MAX * sizeof(bruce_ssh_sftp_entry_t));
    if (entries == NULL) {
        dialog__message(BRUCE_DIALOG_ERROR, "SFTP", "Out of memory.");
        return;
    }

    for (;;) {
        size_t count = 0;
        bruce_result_t result =
            ssh__sftp_list(session, remote_dir, entries, SFTP_APP_ENTRY_MAX, &count, 15000);
        if (result != BRUCE_OK) {
            stdio__printf("SFTP: could not list '%s' (%d)\n", remote_dir, result);
            dialog__message(BRUCE_DIALOG_ERROR, "SFTP", "Could not list that directory.");
            if (strcmp(remote_dir, ".") == 0) break;
            sftp_app__ascend(remote_dir);
            continue;
        }

        bool at_root = strcmp(remote_dir, ".") == 0;
        size_t choice_count = count + (at_root ? 1u : 2u);
        bruce_dialog_choice_t *choices = memory__malloc(choice_count * sizeof(bruce_dialog_choice_t));
        char *right_text = memory__malloc(count * 16u);
        if (choices == NULL || (count > 0 && right_text == NULL)) {
            if (choices != NULL) memory__free(choices);
            if (right_text != NULL) memory__free(right_text);
            dialog__message(BRUCE_DIALOG_ERROR, "SFTP", "Out of memory.");
            break;
        }
        size_t index = 0;
        if (!at_root) choices[index++] = (bruce_dialog_choice_t){.label = "[..]", .icon_name = "folder"};
        for (size_t i = 0; i < count; ++i) {
            char *slot = right_text + i * 16u;
            slot[0] = '\0';
            if (!entries[i].is_directory) snprintf(slot, 16, "%llu B", (unsigned long long)entries[i].size);
            choices[index++] = (bruce_dialog_choice_t){
                .label = entries[i].name,
                .icon_name = entries[i].is_directory ? "folder" : "file-document",
                .right_text = slot,
            };
        }
        choices[index++] = (bruce_dialog_choice_t){.label = "Disconnect", .icon_name = "close"};

        size_t selected = 0;
        result = dialog__choice(remote_dir, NULL, choices, choice_count, &selected);
        memory__free(choices);
        if (right_text != NULL) memory__free(right_text);
        if (result != BRUCE_OK) break; /* Cancelled/Back also disconnects. */

        if (!at_root && selected == 0) {
            sftp_app__ascend(remote_dir);
            continue;
        }
        size_t entry_index = selected - (at_root ? 0u : 1u);
        if (entry_index >= count) break; /* "Disconnect" */

        bruce_ssh_sftp_entry_t *entry = &entries[entry_index];
        if (entry->is_directory) {
            if (sftp_app__descend(remote_dir, sizeof(remote_dir), entry->name) != BRUCE_OK) {
                dialog__message(BRUCE_DIALOG_ERROR, "SFTP", "That path is too long to open.");
            }
            continue;
        }

        char remote_path[SFTP_APP_REMOTE_PATH_MAX];
        if (sftp_app__descend(remote_dir, sizeof(remote_dir), entry->name) != BRUCE_OK) continue;
        snprintf(remote_path, sizeof(remote_path), "%s", remote_dir);
        sftp_app__ascend(remote_dir); /* Undo the temporary descend above. */

        bruce_dialog_choice_t file_actions[] = {
            {.label = "View",     .value = "view"    },
            {.label = "Download", .value = "download"},
            {.label = "Back",     .value = "back"    },
        };
        size_t action_selected = 0;
        if (dialog__choice(entry->name, NULL, file_actions, 3, &action_selected) != BRUCE_OK) continue;
        const char *action = file_actions[action_selected].value;
        if (strcmp(action, "view") == 0) (void)sftp_app__view(session, remote_path, entry->name);
        else if (strcmp(action, "download") == 0) (void)sftp_app__download(session, remote_path, entry->name);
    }
    memory__free(entries);
}

/* Prompts for everything a fresh connection needs: host, port, username,
 * then a choice between a default identity file (if one exists) and a
 * password. Used both for the "New connection..." entry and for `sftp` run
 * with no arguments at all. */
static void sftp_app__new_connection(void) {
    char host[SFTP_APP_HOST_MAX] = "";
    if (dialog__text_input("SFTP", "Host", NULL, false, host, sizeof(host)) != BRUCE_OK || host[0] == '\0') return;

    char port_text[8] = "22";
    if (dialog__text_input("SFTP", "Port", "22", false, port_text, sizeof(port_text)) != BRUCE_OK) return;
    uint16_t port = 22;
    if (!sftp_app__parse_port(port_text, &port)) {
        dialog__message(BRUCE_DIALOG_ERROR, "SFTP", "Invalid port.");
        return;
    }

    char username[SFTP_APP_USERNAME_MAX] = "";
    if (dialog__text_input("SFTP", "Username", NULL, false, username, sizeof(username)) != BRUCE_OK ||
        username[0] == '\0')
        return;

    const char *identity = sftp_app__default_identity();
    if (identity[0] != '\0') {
        bruce_dialog_choice_t choices[] = {
            {.label = "Use default key", .value = "key"     },
            {.label = "Use a password",  .value = "password"},
        };
        size_t selected = 0;
        if (dialog__choice("SFTP", "Authentication", choices, 2, &selected) != BRUCE_OK) return;
        if (strcmp(choices[selected].value, "password") == 0) identity = "";
    }

    bruce_ssh_id_t session = BRUCE_SSH_ID_INVALID;
    if (sftp_app__connect(host, port, username, identity, &session) != BRUCE_OK) return;
    sftp_app__browse(session);
    (void)ssh__close(session);
}

/* Opens a materialized ".sftp" location file under "/Network": empty content means
 * "New connection..." (see sftp_app__list_autodiscover() above); anything
 * else is a host alias resolved through "/.ssh/config" the same way `ssh
 * <alias>` resolves one, prompting for a username/password if the config
 * doesn't supply them. */
static bruce_result_t sftp_app__open_location(const char *path) {
    char location[SFTP_APP_HOST_MAX] = "";
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) == BRUCE_OK) {
        size_t read_size = 0;
        (void)storage__read(file, location, sizeof(location) - 1u, &read_size);
        location[read_size] = '\0';
        storage__close(file);
    }
    char *trimmed = sftp_app__trim(location);

    if (trimmed[0] == '\0') {
        sftp_app__new_connection();
        return BRUCE_OK;
    }

    sftp_app__config_t config = {0};
    bruce_result_t config_result = sftp_app__load_config(trimmed, &config);
    if (config_result != BRUCE_OK) {
        stdio__printf("SFTP client: failed to read %s (%d)\n", SFTP_APP_CONFIG_PATH, config_result);
        return config_result;
    }
    char host[SFTP_APP_HOST_MAX];
    snprintf(host, sizeof(host), "%s", config.hostname_set ? config.hostname : trimmed);
    uint16_t port = config.port_set ? config.port : 22;

    char username[SFTP_APP_USERNAME_MAX];
    if (config.username_set) {
        snprintf(username, sizeof(username), "%s", config.username);
    } else if (dialog__text_input("SFTP", "Username", NULL, false, username, sizeof(username)) != BRUCE_OK ||
               username[0] == '\0') {
        return BRUCE_ERR_CANCELLED;
    }

    const char *identity = config.identity_set ? config.identity : sftp_app__default_identity();

    bruce_ssh_id_t session = BRUCE_SSH_ID_INVALID;
    bruce_result_t result = sftp_app__connect(host, port, username, identity, &session);
    if (result != BRUCE_OK) return result;
    sftp_app__browse(session);
    (void)ssh__close(session);
    return BRUCE_OK;
}

int sftp_app_main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        stdio__printf(
            "SFTP client. Run as \"sftp list --autodiscover\" to print discovered locations "
            "(used by filemanager's \"/Network\" folder), \"sftp <path>\" to open a location file, "
            "or with no arguments to start a new connection.\n"
        );
        return BRUCE_OK;
    }
    if (argc >= 3 && strcmp(argv[1], "list") == 0 && strcmp(argv[2], "--autodiscover") == 0) {
        sftp_app__list_autodiscover();
        return BRUCE_OK;
    }
    if (argc >= 2 && argv[1][0] != '\0') return sftp_app__open_location(argv[1]);
    sftp_app__new_connection();
    return BRUCE_OK;
}
