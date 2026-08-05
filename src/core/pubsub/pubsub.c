#include "core_sdk/pubsub.h"

#include <string.h>

#include "core/process/process.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define PUBSUB__MAX_SUBSCRIPTIONS 8
#define PUBSUB__QUEUE_LENGTH 8

typedef struct {
    bool in_use;
    bruce_pubsub_id_t id;
    bruce_process_id_t owner;
    bruce_resource_id_t resource_id;
    char topic[BRUCE_PUBSUB_TOPIC_MAX];
    QueueHandle_t queue;
} pubsub__slot_t;

static pubsub__slot_t s_slots[PUBSUB__MAX_SUBSCRIPTIONS];
static bruce_pubsub_id_t s_next_id = 1;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void pubsub__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void pubsub__unlock(void) { xSemaphoreGive(s_mutex); }

static bool pubsub__caller_is_builtin(void) {
    bool built_in = false;
    /* No Core process context at all (e.g. a raw background task started
     * during boot, never registered via process_registry__create()) is
     * treated the same as a built-in's implicit grant - same convention as
     * permission__check(). */
    if (process_registry__current_context(&built_in, NULL, 0, NULL) != BRUCE_OK) return true;
    return built_in;
}

static int pubsub__find_locked(bruce_pubsub_id_t id) {
    if (id == BRUCE_PUBSUB_ID_INVALID) return -1;
    for (int i = 0; i < PUBSUB__MAX_SUBSCRIPTIONS; ++i) {
        if (s_slots[i].in_use && s_slots[i].id == id) return i;
    }
    return -1;
}

static void pubsub__release_locked(pubsub__slot_t *slot) {
    QueueHandle_t queue = slot->queue;
    memset(slot, 0, sizeof(*slot));
    if (queue != NULL) vQueueDelete(queue);
}

static void pubsub__cleanup(void *context) {
    pubsub__lock();
    pubsub__slot_t *slot = context;
    if (slot->in_use) pubsub__release_locked(slot);
    pubsub__unlock();
}

static TickType_t pubsub__ticks(uint32_t timeout_ms) {
    if (timeout_ms == 0) return 0;
    if (timeout_ms == UINT32_MAX) return portMAX_DELAY;
    return pdMS_TO_TICKS(timeout_ms);
}

static uint64_t pubsub__now_ms(void) { return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

bruce_result_t pubsub__publish(const char *topic, const void *data, size_t size) {
    if (topic == NULL || topic[0] == '\0' || strlen(topic) >= BRUCE_PUBSUB_TOPIC_MAX ||
        size > BRUCE_PUBSUB_MESSAGE_MAX || (size > 0 && data == NULL)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!pubsub__caller_is_builtin()) return BRUCE_ERR_PERMISSION;

    bruce_pubsub_message_t message = {0};
    if (size > 0) memcpy(message.data, data, size);
    message.size = size;
    message.timestamp_ms = pubsub__now_ms();

    pubsub__lock();
    for (int i = 0; i < PUBSUB__MAX_SUBSCRIPTIONS; ++i) {
        if (!s_slots[i].in_use || strcmp(s_slots[i].topic, topic) != 0) continue;
        if (xQueueSend(s_slots[i].queue, &message, 0) != pdPASS) {
            bruce_pubsub_message_t discard;
            (void)xQueueReceive(s_slots[i].queue, &discard, 0);
            (void)xQueueSend(s_slots[i].queue, &message, 0);
        }
    }
    pubsub__unlock();
    return BRUCE_OK;
}

bruce_result_t pubsub__subscribe(const char *topic, bruce_pubsub_id_t *out_sub) {
    if (out_sub != NULL) *out_sub = BRUCE_PUBSUB_ID_INVALID;
    if (topic == NULL || topic[0] == '\0' || strlen(topic) >= BRUCE_PUBSUB_TOPIC_MAX || out_sub == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    bruce_process_id_t owner = process__current_id();

    QueueHandle_t queue = xQueueCreate(PUBSUB__QUEUE_LENGTH, sizeof(bruce_pubsub_message_t));
    if (queue == NULL) return BRUCE_ERR_NO_MEMORY;

    pubsub__lock();
    int slot_index = -1;
    for (int i = 0; i < PUBSUB__MAX_SUBSCRIPTIONS; ++i) {
        if (!s_slots[i].in_use) {
            slot_index = i;
            break;
        }
    }
    if (slot_index < 0) {
        pubsub__unlock();
        vQueueDelete(queue);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    s_slots[slot_index].in_use = true;
    s_slots[slot_index].queue = queue;
    strncpy(s_slots[slot_index].topic, topic, sizeof(s_slots[slot_index].topic) - 1);
    pubsub__unlock();

    bruce_resource_id_t resource = process_registry__resource_register(pubsub__cleanup, &s_slots[slot_index]);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        pubsub__cleanup(&s_slots[slot_index]);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    pubsub__lock();
    bruce_pubsub_id_t id = s_next_id++;
    if (s_next_id == BRUCE_PUBSUB_ID_INVALID) s_next_id = 1;
    s_slots[slot_index].id = id;
    s_slots[slot_index].owner = owner;
    s_slots[slot_index].resource_id = resource;
    pubsub__unlock();
    *out_sub = id;
    return BRUCE_OK;
}

bruce_result_t pubsub__unsubscribe(bruce_pubsub_id_t sub) {
    bruce_process_id_t owner = process__current_id();
    pubsub__lock();
    int index = pubsub__find_locked(sub);
    if (index < 0 || s_slots[index].owner != owner) {
        pubsub__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    bruce_resource_id_t resource = s_slots[index].resource_id;
    pubsub__release_locked(&s_slots[index]);
    pubsub__unlock();
    return process_registry__resource_release(resource);
}

bruce_result_t pubsub__read(bruce_pubsub_id_t sub, bruce_pubsub_message_t *out_message, uint32_t timeout_ms) {
    if (out_message == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_process_id_t owner = process__current_id();
    pubsub__lock();
    int index = pubsub__find_locked(sub);
    if (index < 0 || s_slots[index].owner != owner) {
        pubsub__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    QueueHandle_t queue = s_slots[index].queue;
    pubsub__unlock();

    return xQueueReceive(queue, out_message, pubsub__ticks(timeout_ms)) == pdPASS ? BRUCE_OK
                                                                                   : BRUCE_ERR_TIMEOUT;
}
