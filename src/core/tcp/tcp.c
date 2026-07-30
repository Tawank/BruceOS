#include "tcp.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "core/network/network.h"
#include "core/process/process.h"
#include "core_sdk/permission.h"
#include "core_sdk/tcp.h"

#define TCP__MAX_SOCKETS 16
#define TCP__DEFAULT_CONNECT_TIMEOUT_MS 10000u

typedef struct {
    bool in_use;
    int fd;
    bruce_tcp_id_t id;
    bruce_resource_id_t resource_id;
    bruce_process_id_t owner;
} tcp__slot_t;

static tcp__slot_t s_slots[TCP__MAX_SOCKETS];
static bruce_tcp_id_t s_next_id = 1;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void tcp__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void tcp__unlock(void) { xSemaphoreGive(s_mutex); }

static int tcp__find_locked(bruce_tcp_id_t id) {
    if (id == BRUCE_TCP_ID_INVALID) return -1;
    for (int i = 0; i < TCP__MAX_SOCKETS; ++i) {
        if (s_slots[i].in_use && s_slots[i].id == id) return i;
    }
    return -1;
}

static void tcp__cleanup(void *context) {
    tcp__slot_t *slot = context;
    tcp__lock();
    if (slot->in_use) {
        close(slot->fd);
        slot->in_use = false;
        slot->id = BRUCE_TCP_ID_INVALID;
    }
    tcp__unlock();
}

static bruce_result_t tcp__adopt(int fd, bruce_tcp_id_t *out_socket) {
    tcp__lock();
    int index = -1;
    for (int i = 0; i < TCP__MAX_SOCKETS; ++i) {
        if (!s_slots[i].in_use) {
            index = i;
            s_slots[i].in_use = true;
            s_slots[i].fd = fd;
            s_slots[i].id = BRUCE_TCP_ID_INVALID;
            break;
        }
    }
    tcp__unlock();
    if (index < 0) {
        close(fd);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    bruce_resource_id_t resource = process_registry__resource_register(tcp__cleanup, &s_slots[index]);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        close(fd);
        tcp__lock();
        s_slots[index].in_use = false;
        tcp__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    tcp__lock();
    bruce_tcp_id_t id = s_next_id++;
    if (s_next_id == BRUCE_TCP_ID_INVALID) s_next_id = 1;
    s_slots[index].id = id;
    s_slots[index].owner = process__current_id();
    s_slots[index].resource_id = resource;
    tcp__unlock();
    *out_socket = id;
    return BRUCE_OK;
}

static bruce_result_t tcp__owned_fd(bruce_tcp_id_t socket, int *out_fd) {
    tcp__lock();
    int index = tcp__find_locked(socket);
    if (index < 0) {
        tcp__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (s_slots[index].owner != process__current_id()) {
        tcp__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    *out_fd = s_slots[index].fd;
    tcp__unlock();
    return BRUCE_OK;
}

static bruce_result_t tcp__wait_fd(int fd, bool write_ready, uint32_t timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
    };
    int ready = select(fd + 1, write_ready ? NULL : &set, write_ready ? &set : NULL, NULL, &timeout);
    if (ready == 0) return BRUCE_ERR_TIMEOUT;
    return ready < 0 ? BRUCE_ERR_IO : BRUCE_OK;
}

bruce_result_t
tcp__connect(const char *host, uint16_t port, uint32_t timeout_ms, bruce_tcp_id_t *out_socket) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (host == NULL || host[0] == '\0' || port == 0 || out_socket == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_socket = BRUCE_TCP_ID_INVALID;
    bruce_result_t network_result = network__init();
    if (network_result != BRUCE_OK) return network_result;

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

    bruce_tcp_id_t socket_id = BRUCE_TCP_ID_INVALID;
    bruce_result_t adopted = tcp__adopt(fd, &socket_id);
    if (adopted != BRUCE_OK) {
        freeaddrinfo(addresses);
        return adopted;
    }

    int connected = connect(fd, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (connected < 0 && errno != EINPROGRESS) {
        (void)tcp__close(socket_id);
        return BRUCE_ERR_IO;
    }
    if (connected < 0) {
        bruce_result_t wait =
            tcp__wait_fd(fd, true, timeout_ms == 0 ? TCP__DEFAULT_CONNECT_TIMEOUT_MS : timeout_ms);
        if (wait != BRUCE_OK) {
            (void)tcp__close(socket_id);
            return wait;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
            (void)tcp__close(socket_id);
            return BRUCE_ERR_IO;
        }
    }
    *out_socket = socket_id;
    return BRUCE_OK;
}

bruce_result_t tcp__listen(uint16_t port, bruce_tcp_id_t *out_listener) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (port == 0 || out_listener == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_listener = BRUCE_TCP_ID_INVALID;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return BRUCE_ERR_IO;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 1) < 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        int error = errno;
        close(fd);
        return error == EADDRINUSE ? BRUCE_ERR_BUSY : BRUCE_ERR_IO;
    }
    return tcp__adopt(fd, out_listener);
}

bruce_result_t tcp__accept(
    bruce_tcp_id_t listener, uint32_t timeout_ms, bruce_tcp_id_t *out_socket, bruce_tcp_endpoint_t *out_peer
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (out_socket == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_socket = BRUCE_TCP_ID_INVALID;
    int fd;
    bruce_result_t result = tcp__owned_fd(listener, &fd);
    if (result != BRUCE_OK) return result;
    result = tcp__wait_fd(fd, false, timeout_ms);
    if (result != BRUCE_OK) return result;

    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    int accepted = accept(fd, (struct sockaddr *)&peer, &peer_size);
    if (accepted < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? BRUCE_ERR_TIMEOUT : BRUCE_ERR_IO;
    int flags = fcntl(accepted, F_GETFL, 0);
    if (flags < 0 || fcntl(accepted, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(accepted);
        return BRUCE_ERR_IO;
    }
    result = tcp__adopt(accepted, out_socket);
    if (result == BRUCE_OK && out_peer != NULL) {
        memset(out_peer, 0, sizeof(*out_peer));
        (void)inet_ntop(AF_INET, &peer.sin_addr, out_peer->host, sizeof(out_peer->host));
        out_peer->port = ntohs(peer.sin_port);
    }
    return result;
}

bruce_result_t
tcp__read(bruce_tcp_id_t socket, void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    int fd;
    bruce_result_t result = tcp__owned_fd(socket, &fd);
    if (result != BRUCE_OK) return result;
    result = tcp__wait_fd(fd, false, timeout_ms);
    if (result != BRUCE_OK) return result;
    ssize_t received = recv(fd, buffer, capacity, 0);
    if (received < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? BRUCE_ERR_TIMEOUT : BRUCE_ERR_IO;
    *out_size = (size_t)received;
    return BRUCE_OK;
}

bruce_result_t
tcp__write(bruce_tcp_id_t socket, const void *buffer, size_t size, uint32_t timeout_ms, size_t *out_size) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if ((buffer == NULL && size != 0) || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    if (size == 0) return BRUCE_OK;
    int fd;
    bruce_result_t result = tcp__owned_fd(socket, &fd);
    if (result != BRUCE_OK) return result;
    result = tcp__wait_fd(fd, true, timeout_ms);
    if (result != BRUCE_OK) return result;
    ssize_t sent = send(fd, buffer, size, 0);
    if (sent < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? BRUCE_ERR_TIMEOUT : BRUCE_ERR_IO;
    *out_size = (size_t)sent;
    return BRUCE_OK;
}

bruce_result_t tcp__close(bruce_tcp_id_t socket) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    tcp__lock();
    int index = tcp__find_locked(socket);
    if (index < 0) {
        tcp__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (s_slots[index].owner != process__current_id()) {
        tcp__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    int fd = s_slots[index].fd;
    bruce_resource_id_t resource = s_slots[index].resource_id;
    s_slots[index].in_use = false;
    s_slots[index].id = BRUCE_TCP_ID_INVALID;
    tcp__unlock();
    close(fd);
    (void)process_registry__resource_release(resource);
    return BRUCE_OK;
}
