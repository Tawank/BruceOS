#include "ssh.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "libssh2.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "core/network/network.h"
#include "core/process/process.h"
#include "core_sdk/permission.h"
#include "core_sdk/runtime.h"
#include "core_sdk/ssh.h"

#define SSH__MAX_SESSIONS 4
#define SSH__DEFAULT_CONNECT_TIMEOUT_MS 10000u

typedef struct {
    bool in_use;
    int fd;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    bool host_key_verified;
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

static void ssh__destroy_handles(LIBSSH2_CHANNEL *channel, LIBSSH2_SESSION *session, int fd) {
    if (channel != NULL) (void)libssh2_channel_free(channel);
    if (session != NULL) {
        (void)libssh2_session_disconnect(session, "BruceOS session closed");
        (void)libssh2_session_free(session);
    }
    if (fd >= 0) close(fd);
}

static void ssh__cleanup(void *context) {
    ssh__slot_t *slot = context;
    ssh__lock();
    LIBSSH2_CHANNEL *channel = slot->channel;
    LIBSSH2_SESSION *session = slot->session;
    int fd = slot->fd;
    slot->in_use = false;
    slot->channel = NULL;
    slot->session = NULL;
    slot->fd = -1;
    slot->id = BRUCE_SSH_ID_INVALID;
    ssh__unlock();
    ssh__destroy_handles(channel, session, fd);
}

static bruce_result_t ssh__reserve(bruce_ssh_id_t *out_id, int *out_index) {
    ssh__lock();
    if (!s_library_initialized) {
        if (libssh2_init(0) != 0) {
            ssh__unlock();
            return BRUCE_ERR_NOT_INITIALIZED;
        }
        s_library_initialized = true;
    }
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
    int directions = libssh2_session_block_directions(slot->session);
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (directions == 0 || (directions & LIBSSH2_SESSION_BLOCK_INBOUND)) FD_SET(slot->fd, &read_set);
    if (directions == 0 || (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND)) FD_SET(slot->fd, &write_set);

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
    LIBSSH2_CHANNEL *channel = s_slots[index].channel;
    LIBSSH2_SESSION *session = s_slots[index].session;
    int fd = s_slots[index].fd;
    bruce_resource_id_t resource = s_slots[index].resource_id;
    s_slots[index].in_use = false;
    s_slots[index].channel = NULL;
    s_slots[index].session = NULL;
    s_slots[index].fd = -1;
    s_slots[index].id = BRUCE_SSH_ID_INVALID;
    ssh__unlock();
    ssh__destroy_handles(channel, session, fd);
    (void)process_registry__resource_release(resource);
    return BRUCE_OK;
}

bruce_result_t ssh__connect(
    const char *host, uint16_t port, uint32_t timeout_ms, bruce_ssh_id_t *out_session
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (host == NULL || host[0] == '\0' || port == 0 || out_session == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_session = BRUCE_SSH_ID_INVALID;
    bruce_result_t result = network__init();
    if (result != BRUCE_OK) return result;

    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned int)port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0 || addresses == NULL) return BRUCE_ERR_NOT_FOUND;

    bruce_ssh_id_t id;
    int index;
    result = ssh__reserve(&id, &index);
    if (result != BRUCE_OK) {
        freeaddrinfo(addresses);
        return result;
    }
    ssh__slot_t *slot = &s_slots[index];
    slot->fd = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    if (slot->fd < 0) {
        freeaddrinfo(addresses);
        (void)ssh__close_internal(id, false);
        return BRUCE_ERR_IO;
    }
    int flags = fcntl(slot->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(slot->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        freeaddrinfo(addresses);
        (void)ssh__close_internal(id, false);
        return BRUCE_ERR_IO;
    }

    uint32_t effective_timeout = timeout_ms == 0 ? SSH__DEFAULT_CONNECT_TIMEOUT_MS : timeout_ms;
    uint64_t deadline = runtime__now() + effective_timeout;
    int connected = connect(slot->fd, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (connected < 0 && errno != EINPROGRESS) result = BRUCE_ERR_IO;
    if (connected < 0 && result == BRUCE_OK) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(slot->fd, &write_set);
        struct timeval timeout = {
            .tv_sec = (time_t)(effective_timeout / 1000u),
            .tv_usec = (suseconds_t)((effective_timeout % 1000u) * 1000u),
        };
        int ready = select(slot->fd + 1, NULL, &write_set, NULL, &timeout);
        int error = 0;
        socklen_t error_size = sizeof(error);
        if (ready == 0) result = BRUCE_ERR_TIMEOUT;
        else if (ready < 0 || getsockopt(slot->fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error != 0)
            result = BRUCE_ERR_IO;
    }
    if (result == BRUCE_OK) {
        slot->session = libssh2_session_init();
        if (slot->session == NULL) result = BRUCE_ERR_NO_MEMORY;
    }
    if (result == BRUCE_OK) {
        libssh2_session_set_blocking(slot->session, 0);
        int rc;
        while ((rc = libssh2_session_handshake(slot->session, slot->fd)) == LIBSSH2_ERROR_EAGAIN) {
            result = ssh__wait_socket(slot, deadline);
            if (result != BRUCE_OK) break;
        }
        if (result == BRUCE_OK && rc != 0) result = BRUCE_ERR_IO;
    }
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
    const char *hash = libssh2_hostkey_hash(slot->session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (hash == NULL) return BRUCE_ERR_IO;
    memcpy(out_fingerprint, hash, BRUCE_SSH_HOST_KEY_SHA256_SIZE);
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
    const char *actual = libssh2_hostkey_hash(slot->session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (actual == NULL) return BRUCE_ERR_IO;
    if (memcmp(actual, expected, BRUCE_SSH_HOST_KEY_SHA256_SIZE) != 0) return BRUCE_ERR_PERMISSION;
    slot->host_key_verified = true;
    return BRUCE_OK;
}

bruce_result_t ssh__authenticate_password(
    bruce_ssh_id_t session, const char *username, const char *password, uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (username == NULL || username[0] == '\0' || password == NULL || strlen(username) > UINT_MAX ||
        strlen(password) > UINT_MAX) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (!slot->host_key_verified) return BRUCE_ERR_INVALID_STATE;
    uint64_t deadline = runtime__now() + timeout_ms;
    int rc;
    while ((rc = libssh2_userauth_password_ex(
                slot->session, username, (unsigned int)strlen(username), password,
                (unsigned int)strlen(password), NULL
            )) == LIBSSH2_ERROR_EAGAIN) {
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) return result;
    }
    return rc == 0 ? BRUCE_OK : BRUCE_ERR_PERMISSION;
}

bruce_result_t ssh__open_shell(
    bruce_ssh_id_t session, const char *terminal_type, uint16_t columns, uint16_t rows,
    uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (terminal_type == NULL || terminal_type[0] == '\0' || columns == 0 || rows == 0 ||
        strlen(terminal_type) > UINT_MAX) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->channel != NULL) return BRUCE_ERR_INVALID_STATE;
    uint64_t deadline = runtime__now() + timeout_ms;
    do {
        slot->channel = libssh2_channel_open_session(slot->session);
        if (slot->channel != NULL) break;
        if (libssh2_session_last_errno(slot->session) != LIBSSH2_ERROR_EAGAIN) return BRUCE_ERR_IO;
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) return result;
    } while (true);

    int rc;
    while ((rc = libssh2_channel_request_pty_ex(
                slot->channel, terminal_type, (unsigned int)strlen(terminal_type), NULL, 0,
                columns, rows, 0, 0
            )) == LIBSSH2_ERROR_EAGAIN) {
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) return result;
    }
    if (rc != 0) return BRUCE_ERR_IO;
    while ((rc = libssh2_channel_shell(slot->channel)) == LIBSSH2_ERROR_EAGAIN) {
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) return result;
    }
    return rc == 0 ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t ssh__resize_pty(
    bruce_ssh_id_t session, uint16_t columns, uint16_t rows, uint32_t timeout_ms
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (columns == 0 || rows == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->channel == NULL) return BRUCE_ERR_INVALID_STATE;
    uint64_t deadline = runtime__now() + timeout_ms;
    int rc;
    while ((rc = libssh2_channel_request_pty_size(slot->channel, columns, rows)) == LIBSSH2_ERROR_EAGAIN) {
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) return result;
    }
    return rc == 0 ? BRUCE_OK : BRUCE_ERR_IO;
}

bruce_result_t ssh__read(
    bruce_ssh_id_t session, bool stderr_stream, void *buffer, size_t capacity,
    uint32_t timeout_ms, size_t *out_size
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->channel == NULL) return BRUCE_ERR_INVALID_STATE;
    uint64_t deadline = runtime__now() + timeout_ms;
    ssize_t received;
    while ((received = libssh2_channel_read_ex(slot->channel, stderr_stream ? SSH_EXTENDED_DATA_STDERR : 0,
                                                buffer, capacity)) == LIBSSH2_ERROR_EAGAIN) {
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) return result;
    }
    if (received < 0) return BRUCE_ERR_IO;
    *out_size = (size_t)received;
    return BRUCE_OK;
}

bruce_result_t ssh__write(
    bruce_ssh_id_t session, const void *buffer, size_t size, uint32_t timeout_ms,
    size_t *out_size
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    if ((buffer == NULL && size != 0) || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    if (size == 0) return BRUCE_OK;
    ssh__slot_t *slot;
    bruce_result_t result = ssh__owned_slot(session, &slot);
    if (result != BRUCE_OK) return result;
    if (slot->channel == NULL) return BRUCE_ERR_INVALID_STATE;
    uint64_t deadline = runtime__now() + timeout_ms;
    ssize_t sent;
    while ((sent = libssh2_channel_write(slot->channel, buffer, size)) == LIBSSH2_ERROR_EAGAIN) {
        result = ssh__wait_socket(slot, deadline);
        if (result != BRUCE_OK) return result;
    }
    if (sent < 0) return BRUCE_ERR_IO;
    *out_size = (size_t)sent;
    return BRUCE_OK;
}

bruce_result_t ssh__close(bruce_ssh_id_t session) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_SSH);
    if (permission != BRUCE_OK) return permission;
    return ssh__close_internal(session, true);
}
