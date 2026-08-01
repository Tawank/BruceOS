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
#include "wolfssl/wolfcrypt/hash.h"

#include "core/network/network.h"
#include "core/process/process.h"
#include "core_sdk/permission.h"
#include "core_sdk/runtime.h"
#include "core_sdk/ssh.h"

static const char *const TAG = "bruce_ssh";

#define SSH__MAX_SESSIONS 4
#define SSH__DEFAULT_CONNECT_TIMEOUT_MS 10000u
#define SSH__HOST_MAX 64u

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
 *   2. ssh__authenticate_password() -- reachable only once the caller has
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
    if (slot == NULL || auth_type != WOLFSSH_USERAUTH_PASSWORD || slot->auth_password == NULL)
        return WOLFSSH_USERAUTH_FAILURE;
    auth_data->sf.password.password = (const byte *)slot->auth_password;
    auth_data->sf.password.passwordSz = (word32)slot->auth_password_len;
    return WOLFSSH_USERAUTH_SUCCESS;
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

bruce_result_t ssh__authenticate_password(
    bruce_ssh_id_t session, const char *username, const char *password, uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (username == NULL || username[0] == '\0' || password == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (!slot->host_key_verified) return BRUCE_ERR_INVALID_STATE;
    if (slot->ssh != NULL) return BRUCE_ERR_INVALID_STATE; /* already authenticated */

    uint32_t effective_timeout = timeout_ms == 0 ? SSH__DEFAULT_CONNECT_TIMEOUT_MS : timeout_ms;
    uint64_t deadline = runtime__now() + effective_timeout;

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

    slot->auth_password = password;
    slot->auth_password_len = strlen(password);

    int rc;
    while ((rc = wolfSSH_connect(slot->ssh)) != WS_SUCCESS) {
        int wolfssh_err = wolfSSH_get_error(slot->ssh);
        if (wolfssh_err != WS_WANT_READ && wolfssh_err != WS_WANT_WRITE) break;
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) break;
    }
    slot->auth_password = NULL;
    slot->auth_password_len = 0;

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
     * ssh__authenticate_password() returns (see the comment there); this
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
