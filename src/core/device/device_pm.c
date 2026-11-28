#include "core/device/device.h"
#include "core/display/display.h"
#include "core/event_loop/event_loop.h"
#include "core_sdk/device.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "hal/hal_gpio.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
#include "driver/gpio.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#endif

#define TAG "device"
#define DEVICE__IDLE_WAIT_MS 1000

/* Woken by any HAL interrupt pin (keys, touch, Select, the modem's RI). */
static StaticSemaphore_t s_wait_storage[DEVICE__WAITERS];
static SemaphoreHandle_t s_wait[DEVICE__WAITERS];

static void IRAM_ATTR device__irq_hook(void) {
    BaseType_t woken = pdFALSE;
    for (int i = 0; i < DEVICE__WAITERS; ++i) {
        if (s_wait[i] != NULL) xSemaphoreGiveFromISR(s_wait[i], &woken);
    }
    if (woken) portYIELD_FROM_ISR();
}

void device__wake_waiter(device__waiter_t waiter) {
    if (s_wait[waiter] != NULL) xSemaphoreGive(s_wait[waiter]);
}

#if CONFIG_FREERTOS_USE_TICKLESS_IDLE

static esp_pm_lock_handle_t s_awake;
static bool s_awake_held;
static volatile bool s_asleep;
static volatile bool s_next_any_input;
static volatile bool s_any_input;
static uint64_t s_slept_at;
static StaticSemaphore_t s_woken_storage;
static SemaphoreHandle_t s_woken;
static portMUX_TYPE s_sleep_lock = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_BRUCE_BUTTON_SELECT_ENABLED
static const hal_pin_t s_select = {.number = CONFIG_BRUCE_BUTTON_SELECT_GPIO, .flags = HAL_PIN_ACTIVE_LOW};
#endif

void device__pm_init(void) {
    for (int i = 0; i < DEVICE__WAITERS; ++i) s_wait[i] = xSemaphoreCreateBinaryStatic(&s_wait_storage[i]);
    s_woken = xSemaphoreCreateBinaryStatic(&s_woken_storage);
    hal_pin_irq_set_hook(device__irq_hook);
    /* Held until Core sees nothing needs the chip awake. No frequency
     * scaling: peripherals clocked from APB keep their timing. */
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "bruce_awake", &s_awake) == ESP_OK) {
        s_awake_held = esp_pm_lock_acquire(s_awake) == ESP_OK;
    }
    esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&config) != ESP_OK) ESP_LOGW(TAG, "automatic light sleep unavailable");
    esp_sleep_enable_gpio_wakeup();
}

bool device__pm_awake(void) { return s_awake_held; }

void device__pm_hold(bool awake) {
    if (s_awake == NULL || awake == s_awake_held) return;
    if (awake) {
        s_awake_held = esp_pm_lock_acquire(s_awake) == ESP_OK;
    } else if (esp_pm_lock_release(s_awake) == ESP_OK) {
        s_awake_held = false;
    }
}

bool device__pm_asleep(void) { return s_asleep; }

void device__pm_next_any_input(bool any_input) { s_next_any_input = any_input; }

bool device__pm_input_since_sleep(void) { return s_any_input && event_loop__last_input_ms() > s_slept_at; }

/* Screen off and input dropped; the chip sleeps whenever nothing is due,
 * so app delays and timers still run. */
bruce_result_t device__pm_sleep(void) {
    portENTER_CRITICAL(&s_sleep_lock);
    bool already = s_asleep;
    s_asleep = true;
    portEXIT_CRITICAL(&s_sleep_lock);
    if (!already) {
        (void)xSemaphoreTake(s_woken, 0);
        s_any_input = s_next_any_input;
        s_next_any_input = false;
        s_slept_at = event_loop__last_input_ms();
        /* After the sleep time, input wakes it and goes to the app. */
        event_loop__set_input_blocked(!s_any_input);
        display__light_off(true);
        device__keyboard_light_apply(false);
        ESP_LOGI(TAG, "asleep");
    }
    (void)xSemaphoreTake(s_woken, portMAX_DELAY);
    xSemaphoreGive(s_woken); /* For any other process waiting in device__sleep(). */
    return BRUCE_OK;
}

void device__pm_wake(bool by_select) {
    portENTER_CRITICAL(&s_sleep_lock);
    bool was = s_asleep;
    s_asleep = false;
    portEXIT_CRITICAL(&s_sleep_lock);
    if (!was) return;
    device__pm_hold(true);
    display__light_off(false);
    device__keyboard_light_apply(true);
    event_loop__note_input();
#if CONFIG_BRUCE_BUTTON_SELECT_ENABLED
    /* The press that woke the device is not input: drop it until the input
     * poller has debounced the release. */
    if (by_select) {
        while (gpio_get_level((gpio_num_t)CONFIG_BRUCE_BUTTON_SELECT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BRUCE_INPUT_DEBOUNCE_MS + 2 * CONFIG_BRUCE_INPUT_POLL_PERIOD_MS + 20));
    }
#else
    (void)by_select;
#endif
    event_loop__set_input_blocked(false);
    ESP_LOGI(TAG, "awake");
    xSemaphoreGive(s_woken);
}

bool device__pm_select_pressed(void) {
#if CONFIG_BRUCE_BUTTON_SELECT_ENABLED
    return gpio_get_level((gpio_num_t)CONFIG_BRUCE_BUTTON_SELECT_GPIO) == 0;
#else
    return false;
#endif
}

bruce_result_t device__wait(device__waiter_t waiter, bool busy, uint32_t busy_ms) {
#if CONFIG_BRUCE_BUTTON_SELECT_ENABLED
    static bool s_select_armed;
    if (waiter == DEVICE__WAITER_INPUT) {
        /* After the input process set the pin up as a button. */
        if (!s_select_armed) s_select_armed = hal_pin_irq_enable(&s_select, true) == BRUCE_OK;
        /* Re-armed only once released, or a held button would wake this at once, over and over. */
        if (!device__pm_select_pressed()) (void)hal_pin_irq_take(&s_select);
    }
#endif
    TickType_t started = xTaskGetTickCount();
    uint32_t wait_ms = busy ? busy_ms : DEVICE__IDLE_WAIT_MS;
    if (xSemaphoreTake(s_wait[waiter], pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        /* Woken early: never faster than the busy period, should a line stay active. */
        TickType_t spent = xTaskGetTickCount() - started;
        TickType_t least = pdMS_TO_TICKS(busy_ms);
        if (spent < least) vTaskDelay(least - spent);
    }
    return process__current_signal() == 0 ? BRUCE_OK : BRUCE_ERR_CANCELLED;
}

#else /* !CONFIG_FREERTOS_USE_TICKLESS_IDLE */

void device__pm_init(void) {
    for (int i = 0; i < DEVICE__WAITERS; ++i) s_wait[i] = xSemaphoreCreateBinaryStatic(&s_wait_storage[i]);
    hal_pin_irq_set_hook(device__irq_hook);
}

bool device__pm_awake(void) { return true; }
void device__pm_hold(bool awake) { (void)awake; }
bool device__pm_asleep(void) { return false; }
void device__pm_next_any_input(bool any_input) { (void)any_input; }
bool device__pm_input_since_sleep(void) { return false; }
bruce_result_t device__pm_sleep(void) { return BRUCE_ERR_UNSUPPORTED; }
void device__pm_wake(bool by_select) { (void)by_select; }
bool device__pm_select_pressed(void) { return false; }

bruce_result_t device__wait(device__waiter_t waiter, bool busy, uint32_t busy_ms) {
    (void)waiter;
    (void)busy;
    return runtime__sleep(busy_ms);
}

#endif

bruce_result_t device__wake(void) {
    device__pm_wake(false);
    return BRUCE_OK;
}
