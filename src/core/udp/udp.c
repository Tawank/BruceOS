#include "udp.h"

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
#include "core_sdk/udp.h"

#define UDP__MAX_SOCKETS 16

typedef struct {
    bool in_use;
    int fd;
    bruce_udp_id_t id;
    bruce_resource_id_t resource_id;
    bruce_process_id_t owner;
} udp__slot_t;

static udp__slot_t s_slots[UDP__MAX_SOCKETS];
static bruce_udp_id_t s_next_id = 1;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void udp__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void udp__unlock(void) { xSemaphoreGive(s_mutex); }

static int udp__find_locked(bruce_udp_id_t id) {
    if (id == BRUCE_UDP_ID_INVALID) return -1;
    for (int i = 0; i < UDP__MAX_SOCKETS; ++i) {
        if (s_slots[i].in_use && s_slots[i].id == id) return i;
    }
    return -1;
}

static void udp__cleanup(void *context) {
    udp__slot_t *slot = context;
    udp__lock();
    if (slot->in_use) {
        close(slot->fd);
        slot->in_use = false;
        slot->id = BRUCE_UDP_ID_INVALID;
    }
    udp__unlock();
}

static bruce_result_t udp__adopt(int fd, bruce_udp_id_t *out_socket) {
    udp__lock();
    int index = -1;
    for (int i = 0; i < UDP__MAX_SOCKETS; ++i) {
        if (!s_slots[i].in_use) {
            index = i;
            s_slots[i].in_use = true;
            s_slots[i].fd = fd;
            s_slots[i].id = BRUCE_UDP_ID_INVALID;
            break;
        }
    }
    udp__unlock();
    if (index < 0) {
        close(fd);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    bruce_resource_id_t resource = process_registry__resource_register(udp__cleanup, &s_slots[index]);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        close(fd);
        udp__lock();
        s_slots[index].in_use = false;
        udp__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    udp__lock();
    bruce_udp_id_t id = s_next_id++;
    if (s_next_id == BRUCE_UDP_ID_INVALID) s_next_id = 1;
    s_slots[index].id = id;
    s_slots[index].owner = process__current_id();
    s_slots[index].resource_id = resource;
    udp__unlock();
    *out_socket = id;
    return BRUCE_OK;
}

static bruce_result_t udp__owned_fd(bruce_udp_id_t socket, int *out_fd) {
    udp__lock();
    int index = udp__find_locked(socket);
    if (index < 0) {
        udp__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (s_slots[index].owner != process__current_id()) {
        udp__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    *out_fd = s_slots[index].fd;
    udp__unlock();
    return BRUCE_OK;
}

static bruce_result_t udp__wait_fd(int fd, bool write_ready, uint32_t timeout_ms) {
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

bruce_result_t udp__open(uint16_t local_port, bruce_udp_id_t *out_socket) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (out_socket == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_socket = BRUCE_UDP_ID_INVALID;
    bruce_result_t network_result = network__init();
    if (network_result != BRUCE_OK) return network_result;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return BRUCE_ERR_IO;

    if (local_port != 0) {
        int reuse = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(local_port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            int error = errno;
            close(fd);
            return error == EADDRINUSE ? BRUCE_ERR_BUSY : BRUCE_ERR_IO;
        }
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return BRUCE_ERR_IO;
    }

    return udp__adopt(fd, out_socket);
}

bruce_result_t udp__send_to(
    bruce_udp_id_t socket, const char *host, uint16_t port, const void *buffer, size_t size,
    uint32_t timeout_ms, size_t *out_size
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (host == NULL || host[0] == '\0' || port == 0 || (buffer == NULL && size != 0) || out_size == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_size = 0;
    if (size == 0) return BRUCE_OK;
    int fd;
    bruce_result_t result = udp__owned_fd(socket, &fd);
    if (result != BRUCE_OK) return result;

    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned int)port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0 || addresses == NULL) return BRUCE_ERR_NOT_FOUND;

    result = udp__wait_fd(fd, true, timeout_ms);
    if (result != BRUCE_OK) {
        freeaddrinfo(addresses);
        return result;
    }

    ssize_t sent = sendto(fd, buffer, size, 0, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (sent < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? BRUCE_ERR_TIMEOUT : BRUCE_ERR_IO;
    *out_size = (size_t)sent;
    return BRUCE_OK;
}

bruce_result_t udp__receive_from(
    bruce_udp_id_t socket, void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size,
    bruce_udp_endpoint_t *out_peer
) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    int fd;
    bruce_result_t result = udp__owned_fd(socket, &fd);
    if (result != BRUCE_OK) return result;
    result = udp__wait_fd(fd, false, timeout_ms);
    if (result != BRUCE_OK) return result;

    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    ssize_t received = recvfrom(fd, buffer, capacity, 0, (struct sockaddr *)&peer, &peer_size);
    if (received < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? BRUCE_ERR_TIMEOUT : BRUCE_ERR_IO;
    *out_size = (size_t)received;
    if (out_peer != NULL) {
        memset(out_peer, 0, sizeof(*out_peer));
        (void)inet_ntop(AF_INET, &peer.sin_addr, out_peer->host, sizeof(out_peer->host));
        out_peer->port = ntohs(peer.sin_port);
    }
    return BRUCE_OK;
}

bruce_result_t udp__close(bruce_udp_id_t socket) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_WIFI);
    if (permission != BRUCE_OK) return permission;
    udp__lock();
    int index = udp__find_locked(socket);
    if (index < 0) {
        udp__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (s_slots[index].owner != process__current_id()) {
        udp__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    int fd = s_slots[index].fd;
    bruce_resource_id_t resource = s_slots[index].resource_id;
    s_slots[index].in_use = false;
    s_slots[index].id = BRUCE_UDP_ID_INVALID;
    udp__unlock();
    close(fd);
    (void)process_registry__resource_release(resource);
    return BRUCE_OK;
}
