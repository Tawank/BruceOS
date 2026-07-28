#include "network.h"

#include <stdbool.h>

#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_network_mutex_storage;
static SemaphoreHandle_t s_network_mutex;
static portMUX_TYPE s_network_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_network_initialized;

bruce_result_t network__init(void) {
    if (s_network_mutex == NULL) {
        portENTER_CRITICAL(&s_network_init_mux);
        if (s_network_mutex == NULL) {
            s_network_mutex = xSemaphoreCreateMutexStatic(&s_network_mutex_storage);
        }
        portEXIT_CRITICAL(&s_network_init_mux);
    }

    xSemaphoreTake(s_network_mutex, portMAX_DELAY);
    if (s_network_initialized) {
        xSemaphoreGive(s_network_mutex);
        return BRUCE_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        xSemaphoreGive(s_network_mutex);
        return BRUCE_ERR_IO;
    }
    s_network_initialized = true;
    xSemaphoreGive(s_network_mutex);
    return BRUCE_OK;
}
