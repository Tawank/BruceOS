#include "ssh.h"

#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "wolfssh/ssh.h"
#include "wolfssl/wolfcrypt/asn_public.h"
#include "wolfssl/wolfcrypt/ecc.h"
#include "wolfssl/wolfcrypt/ed25519.h"
#include "wolfssl/wolfcrypt/hash.h"
#include "wolfssl/wolfcrypt/random.h"

#include "core/network/network.h"
#include "core/process/process.h"
#include "core_sdk/permission.h"
#include "core_sdk/runtime.h"
#include "core_sdk/ssh.h"

static const char *const TAG = "bruce_ssh";

#define SSH__MAX_SESSIONS 4
#define SSH__DEFAULT_CONNECT_TIMEOUT_MS 10000u
#define SSH__HOST_MAX 64u
#define SSH__ECDSA_ALGORITHM "ecdsa-sha2-nistp256"
#define SSH__ECDSA_CURVE "nistp256"
#define SSH__ED25519_ALGORITHM "ssh-ed25519"
#define SSH__OPENSSH_MAGIC "openssh-key-v1"
#define SSH__ECDSA_DER_MAX 160u
#define SSH__ECDSA_BLOB_MAX 128u

/* Bruce's SDK contract is a strict connect -> verify host key -> authenticate
 * sequence, but wolfSSH_connect() drives KEX, userauth, and channel/PTY setup
 * as a single pumped state machine with no pause point in between. This is
 * bridged with two independent TCP connections per session ("Strategy A"):
 *
 *   1. ssh__connect() opens a throwaway connection whose only purpose is to
 *      reach the public-key-check callback and capture the host key's
 *      SHA-256 fingerprint. That callback always rejects on this leg (since
 *      host_key_verified is still false), which aborts wolfSSH_connect()
 *      right after KEX -- before any credentials would be needed. The
 *      throwaway session/socket is torn down immediately after.
 *   2. ssh__authenticate_password() or ssh__authenticate_key() -- reachable
 *      only once the caller has
 *      independently confirmed the fingerprint via ssh__verify_host_key_sha256()
 *      -- opens a second, real connection. Its public-key-check callback now
 *      accepts (same verified fingerprint), and its userauth callback supplies
 *      the caller's credentials, so this single wolfSSH_connect() pump
 *      completes KEX, auth, channel open, and PTY/shell request together.
 *
 * The cost is one extra TCP handshake + KEX per session; the benefit is that
 * ssh__connect() never authenticates before the caller has approved the host
 * key, matching the existing security contract exactly. */

typedef struct {
    bool in_use;
    int fd;
    WOLFSSH *ssh;
    char host[SSH__HOST_MAX];
    uint16_t port;
    bool host_key_captured;
    bool host_key_verified;
    uint8_t host_key_fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE];
    const char *auth_password;
    size_t auth_password_len;
    const uint8_t *auth_private_key;
    size_t auth_private_key_len;
    const uint8_t *auth_public_key;
    size_t auth_public_key_len;
    const char *auth_public_key_type;
    size_t auth_public_key_type_len;
    bruce_ssh_id_t id;
    bruce_resource_id_t resource_id;
    bruce_process_id_t owner;
} ssh__slot_t;

static ssh__slot_t s_slots[SSH__MAX_SESSIONS];
static bruce_ssh_id_t s_next_id = 1;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_library_initialized;
static WOLFSSH_CTX *s_ctx;

static void ssh__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void ssh__unlock(void) { xSemaphoreGive(s_mutex); }

static int ssh__find_locked(bruce_ssh_id_t id) {
    if (id == BRUCE_SSH_ID_INVALID) return -1;
    for (int i = 0; i < SSH__MAX_SESSIONS; ++i) {
        if (s_slots[i].in_use && s_slots[i].id == id) return i;
    }
    return -1;
}

/* Registered once on the shared WOLFSSH_CTX; per-session state is threaded
 * through via wolfSSH_SetPublicKeyCheckCtx()/wolfSSH_SetUserAuthCtx(). */
static int ssh__public_key_check_cb(const byte *pub_key, word32 pub_key_size, void *ctx) {
    ssh__slot_t *slot = ctx;
    if (slot == NULL || wc_Sha256Hash(pub_key, pub_key_size, slot->host_key_fingerprint) != 0) return 1;
    slot->host_key_captured = true;
    return slot->host_key_verified ? 0 : 1; /* wolfSSH: 0 accepts, nonzero rejects */
}

static int ssh__user_auth_cb(byte auth_type, WS_UserAuthData *auth_data, void *ctx) {
    ssh__slot_t *slot = ctx;
    if (slot == NULL) return WOLFSSH_USERAUTH_FAILURE;
    if (auth_type == WOLFSSH_USERAUTH_PASSWORD && slot->auth_password != NULL) {
        auth_data->sf.password.password = (const byte *)slot->auth_password;
        auth_data->sf.password.passwordSz = (word32)slot->auth_password_len;
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    if (auth_type == WOLFSSH_USERAUTH_PUBLICKEY && slot->auth_private_key != NULL &&
        slot->auth_public_key != NULL && slot->auth_public_key_type != NULL) {
        auth_data->sf.publicKey.publicKeyType = (const byte *)slot->auth_public_key_type;
        auth_data->sf.publicKey.publicKeyTypeSz = (word32)slot->auth_public_key_type_len;
        auth_data->sf.publicKey.publicKey = slot->auth_public_key;
        auth_data->sf.publicKey.publicKeySz = (word32)slot->auth_public_key_len;
        auth_data->sf.publicKey.privateKey = slot->auth_private_key;
        auth_data->sf.publicKey.privateKeySz = (word32)slot->auth_private_key_len;
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    return WOLFSSH_USERAUTH_FAILURE;
}

static void ssh__put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static size_t ssh__append_string(uint8_t *out, const void *value, size_t size) {
    ssh__put_u32(out, (uint32_t)size);
    memcpy(out + 4, value, size);
    return size + 4;
}

static bruce_result_t ssh__public_blob(ecc_key *key, uint8_t *out, size_t capacity, size_t *out_size) {
    uint8_t point[65];
    word32 point_size = sizeof(point);
    if (wc_ecc_export_x963(key, point, &point_size) != 0) return BRUCE_ERR_IO;
    size_t required = 12u + sizeof(SSH__ECDSA_ALGORITHM) - 1u + sizeof(SSH__ECDSA_CURVE) - 1u + point_size;
    if (required > capacity) return BRUCE_ERR_RESOURCE_LIMIT;
    size_t offset = 0;
    offset += ssh__append_string(out + offset, SSH__ECDSA_ALGORITHM, sizeof(SSH__ECDSA_ALGORITHM) - 1);
    offset += ssh__append_string(out + offset, SSH__ECDSA_CURVE, sizeof(SSH__ECDSA_CURVE) - 1);
    offset += ssh__append_string(out + offset, point, point_size);
    *out_size = offset;
    return BRUCE_OK;
}

static const char s_base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t ssh__base64_encode(const uint8_t *input, size_t size, char *out) {
    size_t written = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t value = (uint32_t)input[i] << 16;
        if (i + 1 < size) value |= (uint32_t)input[i + 1] << 8;
        if (i + 2 < size) value |= input[i + 2];
        out[written++] = s_base64[(value >> 18) & 0x3f];
        out[written++] = s_base64[(value >> 12) & 0x3f];
        out[written++] = i + 1 < size ? s_base64[(value >> 6) & 0x3f] : '=';
        out[written++] = i + 2 < size ? s_base64[value & 0x3f] : '=';
    }
    return written;
}

static int ssh__base64_value(char value) {
    const char *match = strchr(s_base64, value);
    return match == NULL ? -1 : (int)(match - s_base64);
}

static bruce_result_t ssh__pem_decode(
    const char *pem, size_t pem_size, uint8_t *der, size_t capacity, size_t *out_size
) {
    static const char header[] = "-----BEGIN EC PRIVATE KEY-----";
    static const char footer[] = "-----END EC PRIVATE KEY-----";
    if (pem_size < sizeof(header) + sizeof(footer) - 1 ||
        memcmp(pem, header, sizeof(header) - 1) != 0)
        return BRUCE_ERR_INVALID_ARGUMENT;
    size_t i = sizeof(header) - 1;
    size_t written = 0;
    uint32_t value = 0;
    unsigned bits = 0;
    while (i < pem_size) {
        if (i + sizeof(footer) - 1 <= pem_size && memcmp(pem + i, footer, sizeof(footer) - 1) == 0) {
            *out_size = written;
            return written > 0 ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
        }
        char c = pem[i++];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        if (c == '=') continue;
        int digit = ssh__base64_value(c);
        if (digit < 0) return BRUCE_ERR_INVALID_ARGUMENT;
        value = (value << 6) | (uint32_t)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= capacity) return BRUCE_ERR_RESOURCE_LIMIT;
            der[written++] = (uint8_t)(value >> bits);
            value &= bits == 0 ? 0u : (1u << bits) - 1u;
        }
    }
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static bool ssh__read_u32(const uint8_t *data, size_t size, size_t *offset, uint32_t *out) {
    if (*offset > size || size - *offset < 4u) return false;
    const uint8_t *value = data + *offset;
    *out = ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | value[3];
    *offset += 4u;
    return true;
}

static bool ssh__read_string(
    const uint8_t *data, size_t size, size_t *offset, const uint8_t **out, size_t *out_size
) {
    uint32_t length = 0;
    if (!ssh__read_u32(data, size, offset, &length) || *offset > size || length > size - *offset) {
        return false;
    }
    *out = data + *offset;
    *out_size = length;
    *offset += length;
    return true;
}

static bool ssh__string_equals(const uint8_t *value, size_t size, const char *expected) {
    size_t expected_size = strlen(expected);
    return size == expected_size && memcmp(value, expected, size) == 0;
}

static bruce_result_t ssh__decode_openssh_ed25519(
    const void *private_key, size_t private_key_size, uint8_t *decoded,
    size_t decoded_capacity, size_t *out_decoded_size, uint8_t *public_blob,
    size_t public_capacity, size_t *out_public_size
) {
    byte *decoded_ptr = decoded;
    word32 decoded_size = (word32)decoded_capacity;
    const byte *key_type = NULL;
    word32 key_type_size = 0;
    int rc = wolfSSH_ReadKey_buffer(
        private_key, (word32)private_key_size, WOLFSSH_FORMAT_OPENSSH, &decoded_ptr,
        &decoded_size, &key_type, &key_type_size, NULL
    );
    if (rc != WS_SUCCESS) return BRUCE_ERR_INVALID_ARGUMENT;
    if (decoded_ptr != decoded || !ssh__string_equals(key_type, key_type_size, SSH__ED25519_ALGORITHM)) {
        return BRUCE_ERR_UNSUPPORTED;
    }

    static const uint8_t magic[] = SSH__OPENSSH_MAGIC "\0";
    if (decoded_size < sizeof(magic) - 1u || memcmp(decoded, magic, sizeof(magic) - 1u) != 0) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    size_t offset = sizeof(magic) - 1u;
    const uint8_t *cipher = NULL;
    const uint8_t *kdf = NULL;
    const uint8_t *kdf_options = NULL;
    size_t cipher_size = 0;
    size_t kdf_size = 0;
    size_t kdf_options_size = 0;
    uint32_t key_count = 0;
    if (!ssh__read_string(decoded, decoded_size, &offset, &cipher, &cipher_size) ||
        !ssh__read_string(decoded, decoded_size, &offset, &kdf, &kdf_size) ||
        !ssh__read_string(decoded, decoded_size, &offset, &kdf_options, &kdf_options_size) ||
        !ssh__read_u32(decoded, decoded_size, &offset, &key_count)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!ssh__string_equals(cipher, cipher_size, "none") ||
        !ssh__string_equals(kdf, kdf_size, "none") || kdf_options_size != 0 || key_count != 1) {
        return BRUCE_ERR_UNSUPPORTED;
    }

    const uint8_t *blob = NULL;
    size_t blob_size = 0;
    if (!ssh__read_string(decoded, decoded_size, &offset, &blob, &blob_size) ||
        blob_size > public_capacity) {
        return blob_size > public_capacity ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_ERR_INVALID_ARGUMENT;
    }
    size_t blob_offset = 0;
    const uint8_t *blob_type = NULL;
    const uint8_t *raw_public = NULL;
    size_t blob_type_size = 0;
    size_t raw_public_size = 0;
    if (!ssh__read_string(blob, blob_size, &blob_offset, &blob_type, &blob_type_size) ||
        !ssh__read_string(blob, blob_size, &blob_offset, &raw_public, &raw_public_size) ||
        blob_offset != blob_size ||
        !ssh__string_equals(blob_type, blob_type_size, SSH__ED25519_ALGORITHM) ||
        raw_public_size != ED25519_PUB_KEY_SIZE) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    memcpy(public_blob, blob, blob_size);
    *out_decoded_size = decoded_size;
    *out_public_size = blob_size;
    return BRUCE_OK;
}

static void ssh__destroy_handles(WOLFSSH *ssh, int fd) {
    if (ssh != NULL) {
        (void)wolfSSH_shutdown(ssh);
        wolfSSH_free(ssh);
    }
    if (fd >= 0) close(fd);
}

static void ssh__cleanup(void *context) {
    ssh__slot_t *slot = context;
    ssh__lock();
    WOLFSSH *ssh = slot->ssh;
    int fd = slot->fd;
    slot->in_use = false;
    slot->ssh = NULL;
    slot->fd = -1;
    slot->id = BRUCE_SSH_ID_INVALID;
    ssh__unlock();
    ssh__destroy_handles(ssh, fd);
}

static bruce_result_t ssh__ensure_ctx(void) {
    ssh__lock();
    if (!s_library_initialized) {
        if (wolfSSH_Init() != WS_SUCCESS) {
            ssh__unlock();
            return BRUCE_ERR_NOT_INITIALIZED;
        }
        s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
        if (s_ctx == NULL) {
            ssh__unlock();
            return BRUCE_ERR_NOT_INITIALIZED;
        }
        wolfSSH_CTX_SetPublicKeyCheck(s_ctx, ssh__public_key_check_cb);
        wolfSSH_SetUserAuth(s_ctx, ssh__user_auth_cb);
        s_library_initialized = true;
    }
    ssh__unlock();
    return BRUCE_OK;
}

static bruce_result_t ssh__reserve(bruce_ssh_id_t *out_id, int *out_index) {
    bruce_result_t result = ssh__ensure_ctx();
    if (result != BRUCE_OK) return result;

    ssh__lock();
    int index = -1;
    for (int i = 0; i < SSH__MAX_SESSIONS; ++i) {
        if (!s_slots[i].in_use) {
            index = i;
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].in_use = true;
            s_slots[i].fd = -1;
            s_slots[i].id = s_next_id++;
            if (s_next_id == BRUCE_SSH_ID_INVALID) s_next_id = 1;
            s_slots[i].owner = process__current_id();
            break;
        }
    }
    ssh__unlock();
    if (index < 0) return BRUCE_ERR_RESOURCE_LIMIT;

    bruce_resource_id_t resource = process_registry__resource_register(ssh__cleanup, &s_slots[index]);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        ssh__lock();
        s_slots[index].in_use = false;
        ssh__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    ssh__lock();
    s_slots[index].resource_id = resource;
    *out_id = s_slots[index].id;
    ssh__unlock();
    *out_index = index;
    return BRUCE_OK;
}

static bruce_result_t ssh__owned_slot(bruce_ssh_id_t id, ssh__slot_t **out_slot) {
    ssh__lock();
    int index = ssh__find_locked(id);
    if (index < 0) {
        ssh__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (s_slots[index].owner != process__current_id()) {
        ssh__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    *out_slot = &s_slots[index];
    ssh__unlock();
    return BRUCE_OK;
}

static bruce_result_t ssh__wait_socket(ssh__slot_t *slot, uint64_t deadline) {
    int err = wolfSSH_get_error(slot->ssh);
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (err == WS_WANT_WRITE) FD_SET(slot->fd, &write_set);
    else FD_SET(slot->fd, &read_set);

    uint64_t now = runtime__now();
    if (now >= deadline) return BRUCE_ERR_TIMEOUT;
    uint64_t remaining = deadline - now;
    struct timeval timeout = {
        .tv_sec = (time_t)(remaining / 1000u),
        .tv_usec = (suseconds_t)((remaining % 1000u) * 1000u),
    };
    int ready = select(slot->fd + 1, &read_set, &write_set, NULL, &timeout);
    if (ready == 0) return BRUCE_ERR_TIMEOUT;
    return ready < 0 ? BRUCE_ERR_IO : BRUCE_OK;
}

static bruce_result_t ssh__close_internal(bruce_ssh_id_t id, bool check_owner) {
    ssh__lock();
    int index = ssh__find_locked(id);
    if (index < 0) {
        ssh__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (check_owner && s_slots[index].owner != process__current_id()) {
        ssh__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    WOLFSSH *ssh = s_slots[index].ssh;
    int fd = s_slots[index].fd;
    bruce_resource_id_t resource = s_slots[index].resource_id;
    s_slots[index].in_use = false;
    s_slots[index].ssh = NULL;
    s_slots[index].fd = -1;
    s_slots[index].id = BRUCE_SSH_ID_INVALID;
    ssh__unlock();
    ssh__destroy_handles(ssh, fd);
    (void)process_registry__resource_release(resource);
    return BRUCE_OK;
}

/* Opens a fresh non-blocking TCP connection to host:port, bounded by
 * `deadline`. Used for both connect legs described above. */
static bruce_result_t ssh__connect_socket(const char *host, uint16_t port, uint64_t deadline, int *out_fd) {
    *out_fd = -1;
    bruce_result_t result = network__init();
    if (result != BRUCE_OK) return result;

    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned int)port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0 || addresses == NULL) return BRUCE_ERR_NOT_FOUND;

    int fd = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(addresses);
        return BRUCE_ERR_IO;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        freeaddrinfo(addresses);
        close(fd);
        return BRUCE_ERR_IO;
    }

    int connected = connect(fd, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (connected < 0 && errno != EINPROGRESS) {
        close(fd);
        return BRUCE_ERR_IO;
    }
    if (connected < 0) {
        uint64_t now = runtime__now();
        if (now >= deadline) {
            close(fd);
            return BRUCE_ERR_TIMEOUT;
        }
        uint64_t remaining = deadline - now;
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval timeout = {
            .tv_sec = (time_t)(remaining / 1000u),
            .tv_usec = (suseconds_t)((remaining % 1000u) * 1000u),
        };
        int ready = select(fd + 1, NULL, &write_set, NULL, &timeout);
        int error = 0;
        socklen_t error_size = sizeof(error);
        if (ready == 0) {
            close(fd);
            return BRUCE_ERR_TIMEOUT;
        }
        if (ready < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error != 0) {
            close(fd);
            return BRUCE_ERR_IO;
        }
    }
    *out_fd = fd;
    return BRUCE_OK;
}

bruce_result_t ssh__connect(
    const char *host, uint16_t port, uint32_t timeout_ms, bruce_ssh_id_t *out_session
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (host == NULL || host[0] == '\0' || port == 0 || out_session == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_session = BRUCE_SSH_ID_INVALID;

    bruce_ssh_id_t id;
    int index;
    bruce_result_t result = ssh__reserve(&id, &index);
    if (result != BRUCE_OK) return result;
    ssh__slot_t *slot = &s_slots[index];

    int host_len = snprintf(slot->host, sizeof(slot->host), "%s", host);
    if (host_len < 0 || (size_t)host_len >= sizeof(slot->host)) {
        (void)ssh__close_internal(id, false);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    slot->port = port;

    uint32_t effective_timeout = timeout_ms == 0 ? SSH__DEFAULT_CONNECT_TIMEOUT_MS : timeout_ms;
    uint64_t deadline = runtime__now() + effective_timeout;

    int fd;
    result = ssh__connect_socket(host, port, deadline, &fd);
    if (result != BRUCE_OK) {
        (void)ssh__close_internal(id, false);
        return result;
    }
    slot->fd = fd;

    slot->ssh = wolfSSH_new(s_ctx);
    if (slot->ssh == NULL) {
        (void)ssh__close_internal(id, false);
        return BRUCE_ERR_NO_MEMORY;
    }
    (void)wolfSSH_set_fd(slot->ssh, slot->fd);
    wolfSSH_SetPublicKeyCheckCtx(slot->ssh, slot);
    wolfSSH_SetUserAuthCtx(slot->ssh, slot);
    slot->host_key_verified = false;
    slot->host_key_captured = false;

    int rc;
    /* wolfSSH_connect() never returns WS_WANT_READ/WS_WANT_WRITE itself --
     * a would-block on the underlying non-blocking socket is unconditionally
     * wrapped into the generic WS_FATAL_ERROR at every state in its internal
     * state machine. The actual reason lives in wolfSSH_get_error(), which is
     * the documented way to detect "call me again" vs. a real failure (see
     * the wolfSSH_stream_read() doc comment in wolfssh's src/ssh.c). */
    while ((rc = wolfSSH_connect(slot->ssh)) != WS_SUCCESS) {
        int wolfssh_err = wolfSSH_get_error(slot->ssh);
        if (wolfssh_err != WS_WANT_READ && wolfssh_err != WS_WANT_WRITE) break;
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) break;
    }
    /* Expected outcome: the public-key-check callback rejects once it has
     * captured the fingerprint (host_key_verified is still false at this
     * point), so wolfSSH_connect() aborting with an error here is normal,
     * not a failure -- ssh__host_key_sha256()'s successful capture is. */
    if (result == BRUCE_OK && !slot->host_key_captured) {
        ESP_LOGE(
            TAG, "connect to %s:%u failed before host key exchange (wolfssh rc=%d, err=%d %s)", host,
            (unsigned int)port, rc, wolfSSH_get_error(slot->ssh), wolfSSH_get_error_name(slot->ssh)
        );
        result = BRUCE_ERR_IO;
    }

    /* This leg's connection is single-use; tear it down now regardless of
     * outcome. A fresh one is opened in ssh__authenticate_password(). */
    WOLFSSH *leg_ssh = slot->ssh;
    int leg_fd = slot->fd;
    slot->ssh = NULL;
    slot->fd = -1;
    ssh__destroy_handles(leg_ssh, leg_fd);

    if (result != BRUCE_OK) {
        (void)ssh__close_internal(id, false);
        return result;
    }
    *out_session = id;
    return BRUCE_OK;
}

bruce_result_t ssh__host_key_sha256(
    bruce_ssh_id_t session, uint8_t out_fingerprint[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (out_fingerprint == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (!slot->host_key_captured) return BRUCE_ERR_INVALID_STATE;
    memcpy(out_fingerprint, slot->host_key_fingerprint, BRUCE_SSH_HOST_KEY_SHA256_SIZE);
    return BRUCE_OK;
}

bruce_result_t ssh__verify_host_key_sha256(
    bruce_ssh_id_t session, const uint8_t expected[BRUCE_SSH_HOST_KEY_SHA256_SIZE]
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (expected == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (!slot->host_key_captured) return BRUCE_ERR_INVALID_STATE;
    if (memcmp(slot->host_key_fingerprint, expected, BRUCE_SSH_HOST_KEY_SHA256_SIZE) != 0)
        return BRUCE_ERR_PERMISSION;
    slot->host_key_verified = true;
    return BRUCE_OK;
}

static bruce_result_t ssh__authenticate(ssh__slot_t *slot, const char *username, uint32_t timeout_ms) {
    if (!slot->host_key_verified) return BRUCE_ERR_INVALID_STATE;
    if (slot->ssh != NULL) return BRUCE_ERR_INVALID_STATE; /* already authenticated */

    uint32_t effective_timeout = timeout_ms == 0 ? SSH__DEFAULT_CONNECT_TIMEOUT_MS : timeout_ms;
    uint64_t deadline = runtime__now() + effective_timeout;

    bruce_result_t result;
    int fd;
    result = ssh__connect_socket(slot->host, slot->port, deadline, &fd);
    if (result != BRUCE_OK) return result;

    slot->ssh = wolfSSH_new(s_ctx);
    if (slot->ssh == NULL) {
        close(fd);
        return BRUCE_ERR_NO_MEMORY;
    }
    slot->fd = fd;
    (void)wolfSSH_set_fd(slot->ssh, slot->fd);
    wolfSSH_SetPublicKeyCheckCtx(slot->ssh, slot);
    wolfSSH_SetUserAuthCtx(slot->ssh, slot);

    if (wolfSSH_SetUsername(slot->ssh, username) != WS_SUCCESS) {
        ssh__destroy_handles(slot->ssh, slot->fd);
        slot->ssh = NULL;
        slot->fd = -1;
        return BRUCE_ERR_IO;
    }
    /* wolfSSH negotiates KEX, userauth, channel-open, and the PTY/shell
     * request together inside wolfSSH_connect(), so the channel type must be
     * declared now even though ssh__open_shell() (which just applies the
     * caller's requested PTY size) isn't called until later. */
    (void)wolfSSH_SetChannelType(slot->ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0);

    int rc;
    while ((rc = wolfSSH_connect(slot->ssh)) != WS_SUCCESS) {
        int wolfssh_err = wolfSSH_get_error(slot->ssh);
        if (wolfssh_err != WS_WANT_READ && wolfssh_err != WS_WANT_WRITE) break;
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) break;
    }
    slot->auth_password = NULL;
    slot->auth_password_len = 0;
    slot->auth_private_key = NULL;
    slot->auth_private_key_len = 0;
    slot->auth_public_key = NULL;
    slot->auth_public_key_len = 0;
    slot->auth_public_key_type = NULL;
    slot->auth_public_key_type_len = 0;

    if (result == BRUCE_OK && rc != WS_SUCCESS) {
        ESP_LOGE(
            TAG, "authenticate/channel-setup for %s@%s:%u failed (wolfssh rc=%d, %s)", username, slot->host,
            (unsigned int)slot->port, rc, wolfSSH_get_error_name(slot->ssh)
        );
        result = BRUCE_ERR_PERMISSION;
    }
    if (result != BRUCE_OK) {
        ssh__destroy_handles(slot->ssh, slot->fd);
        slot->ssh = NULL;
        slot->fd = -1;
        return result;
    }
    return BRUCE_OK;
}

bruce_result_t ssh__authenticate_password(
    bruce_ssh_id_t session, const char *username, const char *password, uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (username == NULL || username[0] == '\0' || password == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    slot->auth_password = password;
    slot->auth_password_len = strlen(password);
    result = ssh__authenticate(slot, username, timeout_ms);
    slot->auth_password = NULL;
    slot->auth_password_len = 0;
    return result;
}

static bruce_result_t ssh__generate_ecdsa_keypair(
    char *private_key, size_t private_capacity, size_t *out_private_size,
    char *public_key, size_t public_capacity, size_t *out_public_size
) {
    WC_RNG rng;
    ecc_key key;
    bool rng_ready = false;
    bool key_ready = false;
    bruce_result_t result = BRUCE_ERR_IO;
    uint8_t der[SSH__ECDSA_DER_MAX];
    uint8_t blob[SSH__ECDSA_BLOB_MAX];
    if (wc_InitRng(&rng) != 0) goto cleanup;
    rng_ready = true;
    if (wc_ecc_init(&key) != 0) goto cleanup;
    key_ready = true;
    if (wc_ecc_make_key_ex(&rng, 32, &key, ECC_SECP256R1) != 0) goto cleanup;
    int der_size = wc_EccKeyToDer(&key, der, sizeof(der));
    if (der_size <= 0) goto cleanup;
    size_t blob_size = 0;
    result = ssh__public_blob(&key, blob, sizeof(blob), &blob_size);
    if (result != BRUCE_OK) goto cleanup;

    static const char pem_header[] = "-----BEGIN EC PRIVATE KEY-----\n";
    static const char pem_footer[] = "-----END EC PRIVATE KEY-----\n";
    size_t encoded_size = ((size_t)der_size + 2u) / 3u * 4u;
    size_t line_count = (encoded_size + 63u) / 64u;
    size_t private_size = sizeof(pem_header) - 1u + encoded_size + line_count + sizeof(pem_footer) - 1u;
    size_t public_encoded_size = (blob_size + 2u) / 3u * 4u;
    size_t public_size = sizeof(SSH__ECDSA_ALGORITHM) - 1u + 1u + public_encoded_size;
    if (private_size + 1u > private_capacity || public_size + 1u > public_capacity) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }

    size_t offset = 0;
    memcpy(private_key + offset, pem_header, sizeof(pem_header) - 1u);
    offset += sizeof(pem_header) - 1u;
    char encoded[256];
    ssh__base64_encode(der, (size_t)der_size, encoded);
    for (size_t i = 0; i < encoded_size; i += 64u) {
        size_t chunk = encoded_size - i > 64u ? 64u : encoded_size - i;
        memcpy(private_key + offset, encoded + i, chunk);
        offset += chunk;
        private_key[offset++] = '\n';
    }
    memcpy(private_key + offset, pem_footer, sizeof(pem_footer) - 1u);
    offset += sizeof(pem_footer) - 1u;
    private_key[offset] = '\0';
    *out_private_size = offset;

    memcpy(public_key, SSH__ECDSA_ALGORITHM, sizeof(SSH__ECDSA_ALGORITHM) - 1u);
    public_key[sizeof(SSH__ECDSA_ALGORITHM) - 1u] = ' ';
    offset = sizeof(SSH__ECDSA_ALGORITHM);
    offset += ssh__base64_encode(blob, blob_size, public_key + offset);
    public_key[offset] = '\0';
    *out_public_size = offset;
    result = BRUCE_OK;

cleanup:
    memset(der, 0, sizeof(der));
    if (key_ready) wc_ecc_free(&key);
    if (rng_ready) wc_FreeRng(&rng);
    return result;
}

static bool ssh__append_bytes(
    uint8_t *out, size_t capacity, size_t *offset, const void *value, size_t size
) {
    if (*offset > capacity || size > capacity - *offset) return false;
    if (size == 0) return true;
    memcpy(out + *offset, value, size);
    *offset += size;
    return true;
}

static bool ssh__append_u32(uint8_t *out, size_t capacity, size_t *offset, uint32_t value) {
    if (*offset > capacity || capacity - *offset < 4u) return false;
    ssh__put_u32(out + *offset, value);
    *offset += 4u;
    return true;
}

static bool ssh__append_binary_string(
    uint8_t *out, size_t capacity, size_t *offset, const void *value, size_t size
) {
    return size <= UINT32_MAX && ssh__append_u32(out, capacity, offset, (uint32_t)size) &&
           ssh__append_bytes(out, capacity, offset, value, size);
}

static bruce_result_t ssh__generate_ed25519_keypair(
    char *private_key, size_t private_capacity, size_t *out_private_size,
    char *public_key, size_t public_capacity, size_t *out_public_size
) {
    WC_RNG rng;
    ed25519_key key;
    bool rng_ready = false;
    bool key_ready = false;
    bruce_result_t result = BRUCE_ERR_IO;
    uint8_t raw_private[ED25519_PRV_KEY_SIZE];
    uint8_t raw_public[ED25519_PUB_KEY_SIZE];
    uint8_t public_blob[64];
    uint8_t private_section[160];
    uint8_t container[256];
    word32 private_size = sizeof(raw_private);
    word32 public_size = sizeof(raw_public);

    if (wc_InitRng(&rng) != 0) goto cleanup;
    rng_ready = true;
    if (wc_ed25519_init(&key) != 0) goto cleanup;
    key_ready = true;
    if (wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key) != 0 ||
        wc_ed25519_export_key(&key, raw_private, &private_size, raw_public, &public_size) != 0 ||
        private_size != ED25519_PRV_KEY_SIZE || public_size != ED25519_PUB_KEY_SIZE) {
        goto cleanup;
    }

    size_t public_blob_size = 0;
    if (!ssh__append_binary_string(
            public_blob, sizeof(public_blob), &public_blob_size,
            SSH__ED25519_ALGORITHM, sizeof(SSH__ED25519_ALGORITHM) - 1u
        ) ||
        !ssh__append_binary_string(
            public_blob, sizeof(public_blob), &public_blob_size, raw_public, sizeof(raw_public)
        )) {
        result = BRUCE_ERR_INTERNAL;
        goto cleanup;
    }

    uint8_t check_bytes[4];
    if (wc_RNG_GenerateBlock(&rng, check_bytes, sizeof(check_bytes)) != 0) goto cleanup;
    uint32_t check = ((uint32_t)check_bytes[0] << 24) | ((uint32_t)check_bytes[1] << 16) |
                     ((uint32_t)check_bytes[2] << 8) | check_bytes[3];
    size_t section_size = 0;
    if (!ssh__append_u32(private_section, sizeof(private_section), &section_size, check) ||
        !ssh__append_u32(private_section, sizeof(private_section), &section_size, check) ||
        !ssh__append_binary_string(
            private_section, sizeof(private_section), &section_size,
            SSH__ED25519_ALGORITHM, sizeof(SSH__ED25519_ALGORITHM) - 1u
        ) ||
        !ssh__append_binary_string(
            private_section, sizeof(private_section), &section_size, raw_public, sizeof(raw_public)
        ) ||
        !ssh__append_binary_string(
            private_section, sizeof(private_section), &section_size, raw_private, sizeof(raw_private)
        ) ||
        !ssh__append_binary_string(private_section, sizeof(private_section), &section_size, NULL, 0)) {
        result = BRUCE_ERR_INTERNAL;
        goto cleanup;
    }
    uint8_t padding = 1;
    while (section_size % 8u != 0) {
        if (!ssh__append_bytes(private_section, sizeof(private_section), &section_size, &padding, 1)) {
            result = BRUCE_ERR_INTERNAL;
            goto cleanup;
        }
        padding++;
    }

    static const uint8_t magic[] = SSH__OPENSSH_MAGIC "\0";
    static const char none[] = "none";
    size_t container_size = 0;
    if (!ssh__append_bytes(container, sizeof(container), &container_size, magic, sizeof(magic) - 1u) ||
        !ssh__append_binary_string(container, sizeof(container), &container_size, none, sizeof(none) - 1u) ||
        !ssh__append_binary_string(container, sizeof(container), &container_size, none, sizeof(none) - 1u) ||
        !ssh__append_binary_string(container, sizeof(container), &container_size, NULL, 0) ||
        !ssh__append_u32(container, sizeof(container), &container_size, 1) ||
        !ssh__append_binary_string(
            container, sizeof(container), &container_size, public_blob, public_blob_size
        ) ||
        !ssh__append_binary_string(
            container, sizeof(container), &container_size, private_section, section_size
        )) {
        result = BRUCE_ERR_INTERNAL;
        goto cleanup;
    }

    static const char private_header[] = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
    static const char private_footer[] = "-----END OPENSSH PRIVATE KEY-----\n";
    size_t encoded_size = (container_size + 2u) / 3u * 4u;
    size_t line_count = (encoded_size + 69u) / 70u;
    size_t required_private = sizeof(private_header) - 1u + encoded_size + line_count +
                              sizeof(private_footer) - 1u;
    size_t public_encoded_size = (public_blob_size + 2u) / 3u * 4u;
    size_t required_public = sizeof(SSH__ED25519_ALGORITHM) + public_encoded_size;
    if (required_private + 1u > private_capacity || required_public + 1u > public_capacity) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }

    char encoded[352];
    size_t actual_encoded = ssh__base64_encode(container, container_size, encoded);
    size_t offset = 0;
    memcpy(private_key + offset, private_header, sizeof(private_header) - 1u);
    offset += sizeof(private_header) - 1u;
    for (size_t i = 0; i < actual_encoded; i += 70u) {
        size_t chunk = actual_encoded - i > 70u ? 70u : actual_encoded - i;
        memcpy(private_key + offset, encoded + i, chunk);
        offset += chunk;
        private_key[offset++] = '\n';
    }
    memcpy(private_key + offset, private_footer, sizeof(private_footer) - 1u);
    offset += sizeof(private_footer) - 1u;
    private_key[offset] = '\0';
    *out_private_size = offset;

    memcpy(public_key, SSH__ED25519_ALGORITHM, sizeof(SSH__ED25519_ALGORITHM) - 1u);
    public_key[sizeof(SSH__ED25519_ALGORITHM) - 1u] = ' ';
    offset = sizeof(SSH__ED25519_ALGORITHM);
    offset += ssh__base64_encode(public_blob, public_blob_size, public_key + offset);
    public_key[offset] = '\0';
    *out_public_size = offset;
    result = BRUCE_OK;

cleanup:
    memset(raw_private, 0, sizeof(raw_private));
    memset(private_section, 0, sizeof(private_section));
    memset(container, 0, sizeof(container));
    if (key_ready) wc_ed25519_free(&key);
    if (rng_ready) wc_FreeRng(&rng);
    return result;
}

bruce_result_t ssh__generate_keypair_ex(
    bruce_ssh_key_type_t type, char *private_key, size_t private_capacity,
    size_t *out_private_size, char *public_key, size_t public_capacity,
    size_t *out_public_size
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (private_key == NULL || out_private_size == NULL || public_key == NULL || out_public_size == NULL)
        return BRUCE_ERR_INVALID_ARGUMENT;
    *out_private_size = 0;
    *out_public_size = 0;
    if (type == BRUCE_SSH_KEY_ECDSA_P256) {
        return ssh__generate_ecdsa_keypair(
            private_key, private_capacity, out_private_size,
            public_key, public_capacity, out_public_size
        );
    }
    if (type == BRUCE_SSH_KEY_ED25519) {
        return ssh__generate_ed25519_keypair(
            private_key, private_capacity, out_private_size,
            public_key, public_capacity, out_public_size
        );
    }
    return BRUCE_ERR_INVALID_ARGUMENT;
}

bruce_result_t ssh__authenticate_key(
    bruce_ssh_id_t session, const char *username, const void *private_key,
    size_t private_key_size, uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (username == NULL || username[0] == '\0' || private_key == NULL || private_key_size == 0)
        return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;

    uint8_t decoded[BRUCE_SSH_PRIVATE_KEY_MAX_SIZE];
    size_t decoded_size = 0;
    uint8_t blob[SSH__ECDSA_BLOB_MAX];
    size_t blob_size = 0;
    const char *algorithm = NULL;
    size_t algorithm_size = 0;

    static const char openssh_header[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
    bool openssh = private_key_size >= sizeof(openssh_header) - 1u &&
                   memcmp(private_key, openssh_header, sizeof(openssh_header) - 1u) == 0;
    if (openssh) {
        result = ssh__decode_openssh_ed25519(
            private_key, private_key_size, decoded, sizeof(decoded), &decoded_size,
            blob, sizeof(blob), &blob_size
        );
        algorithm = SSH__ED25519_ALGORITHM;
        algorithm_size = sizeof(SSH__ED25519_ALGORITHM) - 1u;
    } else {
        result = ssh__pem_decode(
            private_key, private_key_size, decoded, SSH__ECDSA_DER_MAX, &decoded_size
        );
        if (result == BRUCE_OK) {
            ecc_key key;
            if (wc_ecc_init(&key) != 0) result = BRUCE_ERR_IO;
            else {
                word32 index = 0;
                if (wc_EccPrivateKeyDecode(decoded, &index, &key, (word32)decoded_size) != 0)
                    result = BRUCE_ERR_INVALID_ARGUMENT;
                else
                    result = ssh__public_blob(&key, blob, sizeof(blob), &blob_size);
                wc_ecc_free(&key);
            }
        }
        algorithm = SSH__ECDSA_ALGORITHM;
        algorithm_size = sizeof(SSH__ECDSA_ALGORITHM) - 1u;
    }
    if (result == BRUCE_OK) {
        slot->auth_private_key = decoded;
        slot->auth_private_key_len = decoded_size;
        slot->auth_public_key = blob;
        slot->auth_public_key_len = blob_size;
        slot->auth_public_key_type = algorithm;
        slot->auth_public_key_type_len = algorithm_size;
        result = ssh__authenticate(slot, username, timeout_ms);
    }
    slot->auth_private_key = NULL;
    slot->auth_private_key_len = 0;
    slot->auth_public_key = NULL;
    slot->auth_public_key_len = 0;
    slot->auth_public_key_type = NULL;
    slot->auth_public_key_type_len = 0;
    memset(decoded, 0, sizeof(decoded));
    memset(blob, 0, sizeof(blob));
    return result;
}

bruce_result_t ssh__open_shell(
    bruce_ssh_id_t session, const char *terminal_type, uint16_t columns, uint16_t rows, uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (terminal_type == NULL || terminal_type[0] == '\0' || columns == 0 || rows == 0)
        return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->ssh == NULL) return BRUCE_ERR_INVALID_STATE;
    /* The shell/PTY channel is already open by the time
     * authentication returns (see the comment there); this
     * just applies the caller's requested size. wolfSSH's public API has no
     * separate control for the terminal-type string, so it's accepted for
     * SDK-contract compatibility but not otherwise used. */
    (void)terminal_type;
    (void)timeout_ms;
    if (wolfSSH_ChangeTerminalSize(slot->ssh, columns, rows, 0, 0) != WS_SUCCESS) return BRUCE_ERR_IO;
    return BRUCE_OK;
}

bruce_result_t ssh__resize_pty(bruce_ssh_id_t session, uint16_t columns, uint16_t rows, uint32_t timeout_ms) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (columns == 0 || rows == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->ssh == NULL) return BRUCE_ERR_INVALID_STATE;
    (void)timeout_ms;
    if (wolfSSH_ChangeTerminalSize(slot->ssh, columns, rows, 0, 0) != WS_SUCCESS) return BRUCE_ERR_IO;
    return BRUCE_OK;
}

bruce_result_t ssh__read(
    bruce_ssh_id_t session, bool stderr_stream, void *buffer, size_t capacity, uint32_t timeout_ms,
    size_t *out_size
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->ssh == NULL) return BRUCE_ERR_INVALID_STATE;
    /* wolfSSH's stream API reads whatever the pty channel delivers; there is
     * no separate extended-data (stderr) stream at this level. */
    (void)stderr_stream;
    uint64_t deadline = runtime__now() + timeout_ms;
    for (;;) {
        int received = wolfSSH_stream_read(slot->ssh, buffer, (word32)capacity);
        if (received < 0) {
            /* Like wolfSSH_connect(), wolfSSH_stream_read()'s underlying
             * GetInputData() wraps a would-block into the generic
             * WS_FATAL_ERROR and stashes the real reason in ssh->error --
             * the return value itself is not reliably WS_WANT_READ/WRITE. */
            int wolfssh_err = wolfSSH_get_error(slot->ssh);
            if (wolfssh_err == WS_WANT_READ || wolfssh_err == WS_WANT_WRITE) {
                result = ssh__wait_socket(slot, deadline);
                if (result != BRUCE_OK) return result;
                continue;
            }
            if (received == WS_EOF || wolfssh_err == WS_EOF) return BRUCE_OK;
            return BRUCE_ERR_IO;
        }
        *out_size = (size_t)received;
        return BRUCE_OK;
    }
}

bruce_result_t ssh__write(
    bruce_ssh_id_t session, const void *buffer, size_t size, uint32_t timeout_ms, size_t *out_size
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if ((buffer == NULL && size != 0) || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    if (size == 0) return BRUCE_OK;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->ssh == NULL) return BRUCE_ERR_INVALID_STATE;
    uint64_t deadline = runtime__now() + timeout_ms;
    for (;;) {
        int sent = wolfSSH_stream_send(slot->ssh, (byte *)(uintptr_t)buffer, (word32)size);
        if (sent < 0) {
            int wolfssh_err = wolfSSH_get_error(slot->ssh);
            if (wolfssh_err == WS_WANT_READ || wolfssh_err == WS_WANT_WRITE) {
                result = ssh__wait_socket(slot, deadline);
                if (result != BRUCE_OK) return result;
                continue;
            }
            return BRUCE_ERR_IO;
        }
        *out_size = (size_t)sent;
        return BRUCE_OK;
    }
}

bruce_result_t ssh__close(bruce_ssh_id_t session) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    return ssh__close_internal(session, true);
}
