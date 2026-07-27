#include "core_sdk/bluetooth_hid.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "core/bluetooth/bluetooth_internal.h"
#include "core/task/task.h"
#include "core_sdk/input.h"
#include "core_sdk/notification.h"
#include "core_sdk/permission.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED
#include "esp_gap_bt_api.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#endif

#define BLUETOOTH_HID__DEFAULT_TIMEOUT_MS 10000
#define BLUETOOTH_HID__MAX_TIMEOUT_MS 30000

static uint8_t s_keyboard_usages[6];
static int32_t s_keyboard_codes[6];
static uint16_t s_gamepad_buttons;
static uint8_t s_gamepad_hat = 8;
static int8_t s_gamepad_axes[4];
static bool s_gamepad_initialized;

static bruce_result_t bluetooth_hid__inject(bruce_input_type_t type, bruce_input_action_t action,
                                            int32_t code, int32_t value)
{
    bruce_input_event_t event = {
        .type = type,
        .action = action,
        .code = code,
        .value = value,
    };
    return input__inject(&event);
}

static int32_t bluetooth_hid__keyboard_code(uint8_t usage, bool shift)
{
    if (usage >= 0x04 && usage <= 0x1d) {
        return (shift ? 'A' : 'a') + usage - 0x04;
    }
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    if (usage >= 0x1e && usage <= 0x27) {
        return shift ? shifted_digits[usage - 0x1e] : digits[usage - 0x1e];
    }
    switch (usage) {
    case 0x28: return BRUCE_INPUT_CODE_SELECT;
    case 0x29: return BRUCE_INPUT_CODE_BACK;
    case 0x2a: return '\b';
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x4f: return BRUCE_INPUT_CODE_RIGHT;
    case 0x50: return BRUCE_INPUT_CODE_LEFT;
    case 0x51: return BRUCE_INPUT_CODE_DOWN;
    case 0x52: return BRUCE_INPUT_CODE_UP;
    default: return 0;
    }
}

void bluetooth_hid__reset_input_state(void)
{
    memset(s_keyboard_usages, 0, sizeof(s_keyboard_usages));
    memset(s_keyboard_codes, 0, sizeof(s_keyboard_codes));
    s_gamepad_buttons = 0;
    s_gamepad_hat = 8;
    memset(s_gamepad_axes, 0, sizeof(s_gamepad_axes));
    s_gamepad_initialized = false;
}

bruce_result_t bluetooth_hid__translate_keyboard_report(const uint8_t *data, size_t length)
{
    if (data == NULL || length < 8) return BRUCE_ERR_INVALID_ARGUMENT;
    for (size_t i = 2; i < 8; ++i) {
        if (data[i] == 1) return BRUCE_OK;
    }
    bool shift = (data[0] & 0x22) != 0;
    bruce_result_t result = BRUCE_OK;

    for (size_t old = 0; old < 6; ++old) {
        if (s_keyboard_usages[old] == 0) continue;
        bool retained = false;
        for (size_t current = 0; current < 6; ++current) {
            if (data[current + 2] == s_keyboard_usages[old]) retained = true;
        }
        if (!retained && s_keyboard_codes[old] != 0) {
            bruce_result_t injected = bluetooth_hid__inject(BRUCE_INPUT_KEY, BRUCE_INPUT_RELEASE,
                                                            s_keyboard_codes[old], 0);
            if (result == BRUCE_OK && injected != BRUCE_OK) result = injected;
        }
    }

    uint8_t next_usages[6] = {0};
    int32_t next_codes[6] = {0};
    for (size_t current = 0; current < 6; ++current) {
        uint8_t usage = data[current + 2];
        next_usages[current] = usage;
        if (usage == 0 || usage == 1) continue;
        bool retained = false;
        for (size_t old = 0; old < 6; ++old) {
            if (s_keyboard_usages[old] == usage) {
                retained = true;
                next_codes[current] = s_keyboard_codes[old];
                break;
            }
        }
        if (!retained) {
            next_codes[current] = bluetooth_hid__keyboard_code(usage, shift);
            if (next_codes[current] != 0) {
                bruce_result_t injected = bluetooth_hid__inject(BRUCE_INPUT_KEY, BRUCE_INPUT_PRESS,
                                                                next_codes[current], next_codes[current]);
                if (result == BRUCE_OK && injected != BRUCE_OK) result = injected;
            }
        }
    }
    memcpy(s_keyboard_usages, next_usages, sizeof(s_keyboard_usages));
    memcpy(s_keyboard_codes, next_codes, sizeof(s_keyboard_codes));
    return result;
}

static void bluetooth_hid__inject_hat(uint8_t hat, bruce_input_action_t action)
{
    if (hat == 0 || hat == 1 || hat == 7) {
        (void)bluetooth_hid__inject(BRUCE_INPUT_BUTTON, action, BRUCE_INPUT_CODE_UP,
                                    action == BRUCE_INPUT_PRESS);
    }
    if (hat >= 1 && hat <= 3) {
        (void)bluetooth_hid__inject(BRUCE_INPUT_BUTTON, action, BRUCE_INPUT_CODE_RIGHT,
                                    action == BRUCE_INPUT_PRESS);
    }
    if (hat >= 3 && hat <= 5) {
        (void)bluetooth_hid__inject(BRUCE_INPUT_BUTTON, action, BRUCE_INPUT_CODE_DOWN,
                                    action == BRUCE_INPUT_PRESS);
    }
    if (hat >= 5 && hat <= 7) {
        (void)bluetooth_hid__inject(BRUCE_INPUT_BUTTON, action, BRUCE_INPUT_CODE_LEFT,
                                    action == BRUCE_INPUT_PRESS);
    }
}

bruce_result_t bluetooth_hid__translate_gamepad_report(const uint8_t *data, size_t length)
{
    /* Common HID gamepad report: X, Y, RX, RY, hat, then 16 button bits.
     * Descriptor-specific layouts are deliberately not guessed. */
    if (data == NULL || length < 7) return BRUCE_ERR_INVALID_ARGUMENT;
    static const int32_t axis_codes[4] = {
        BRUCE_INPUT_CODE_GAMEPAD_AXIS_X, BRUCE_INPUT_CODE_GAMEPAD_AXIS_Y,
        BRUCE_INPUT_CODE_GAMEPAD_AXIS_RX, BRUCE_INPUT_CODE_GAMEPAD_AXIS_RY,
    };
    static const int32_t button_codes[12] = {
        BRUCE_INPUT_CODE_BUTTON_A, BRUCE_INPUT_CODE_BUTTON_B, BRUCE_INPUT_CODE_BUTTON_X,
        BRUCE_INPUT_CODE_BUTTON_Y, BRUCE_INPUT_CODE_BUTTON_L1, BRUCE_INPUT_CODE_BUTTON_R1,
        BRUCE_INPUT_CODE_BUTTON_L2, BRUCE_INPUT_CODE_BUTTON_R2, BRUCE_INPUT_CODE_BUTTON_SELECT,
        BRUCE_INPUT_CODE_BUTTON_START, BRUCE_INPUT_CODE_BUTTON_THUMB_L, BRUCE_INPUT_CODE_BUTTON_THUMB_R,
    };

    for (size_t i = 0; i < 4; ++i) {
        int8_t value = (int8_t)data[i];
        if (!s_gamepad_initialized || value != s_gamepad_axes[i]) {
            (void)bluetooth_hid__inject(BRUCE_INPUT_BUTTON, BRUCE_INPUT_CHANGE, axis_codes[i], value);
            s_gamepad_axes[i] = value;
        }
    }
    uint8_t hat = data[4] & 0x0f;
    if (!s_gamepad_initialized || hat != s_gamepad_hat) {
        if (s_gamepad_hat <= 7) bluetooth_hid__inject_hat(s_gamepad_hat, BRUCE_INPUT_RELEASE);
        if (hat <= 7) bluetooth_hid__inject_hat(hat, BRUCE_INPUT_PRESS);
        s_gamepad_hat = hat;
    }

    uint16_t buttons = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
    uint16_t changed = buttons ^ s_gamepad_buttons;
    for (size_t i = 0; i < sizeof(button_codes) / sizeof(button_codes[0]); ++i) {
        uint16_t mask = (uint16_t)1 << i;
        if ((changed & mask) != 0) {
            bool pressed = (buttons & mask) != 0;
            (void)bluetooth_hid__inject(BRUCE_INPUT_BUTTON,
                                        pressed ? BRUCE_INPUT_PRESS : BRUCE_INPUT_RELEASE,
                                        button_codes[i], pressed);
        }
    }
    s_gamepad_buttons = buttons;
    s_gamepad_initialized = true;
    return BRUCE_OK;
}

#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED

#define BLUETOOTH_HID__SCAN_DONE_BIT BIT0
#define BLUETOOTH_HID__OPEN_DONE_BIT BIT1
#define BLUETOOTH_HID__CLOSE_DONE_BIT BIT2
#define BLUETOOTH_HID__START_DONE_BIT BIT3
#define BLUETOOTH_HID__MAX_RESULTS 64

typedef struct {
    esp_hidh_dev_t *dev;
    bruce_resource_id_t resource;
    bruce_task_id_t owner;
    size_t refs;
    bluetooth_hid__device_t snapshot;
    bool keyboard_supported;
    uint8_t keyboard_map;
    uint16_t keyboard_report_id;
    bool gamepad_supported;
    uint8_t gamepad_map;
    uint16_t gamepad_report_id;
} bluetooth_hid__connection_t;

static StaticSemaphore_t s_operation_mutex_storage;
static SemaphoreHandle_t s_operation_mutex;
static EventGroupHandle_t s_events;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_hidh_initialized;
static esp_err_t s_open_status;
static bluetooth_hid__device_t *s_scan_devices;
static size_t s_scan_capacity;
static size_t s_scan_count;
static bool s_scanning;
static bluetooth_hid__connection_t *s_pending_connection;
static bluetooth_hid__connection_t *s_connection;
static esp_hidh_dev_t *s_wait_close_dev;

static void bluetooth_hid__connection_unref(bluetooth_hid__connection_t *connection)
{
    bool destroy = false;
    portENTER_CRITICAL(&s_state_mux);
    if (connection != NULL && connection->refs > 0) {
        connection->refs--;
        destroy = connection->refs == 0;
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (destroy) free(connection);
}

static void bluetooth_hid__operation_lock(void)
{
    if (s_operation_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_operation_mutex == NULL) s_operation_mutex = xSemaphoreCreateMutexStatic(&s_operation_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_operation_mutex, portMAX_DELAY);
}

static void bluetooth_hid__operation_unlock(void)
{
    xSemaphoreGive(s_operation_mutex);
}

static bluetooth_hid__usage_t bluetooth_hid__map_usage(esp_hid_usage_t usage)
{
    if (usage == ESP_HID_USAGE_KEYBOARD) return BRUCE_BLUETOOTH_HID_KEYBOARD;
    if (usage == ESP_HID_USAGE_GAMEPAD || usage == ESP_HID_USAGE_JOYSTICK) return BRUCE_BLUETOOTH_HID_GAMEPAD;
    return BRUCE_BLUETOOTH_HID_UNKNOWN;
}

static void bluetooth_hid__copy_name(char *destination, size_t size, const uint8_t *source, size_t length)
{
    size_t copy_length = length < size - 1 ? length : size - 1;
    if (source != NULL && copy_length > 0) memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

static void bluetooth_hid__gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *parameter)
{
    if (event == ESP_BT_GAP_DISC_RES_EVT && s_scanning) {
        uint32_t cod_value = 0;
        int8_t rssi = -128;
        const uint8_t *name = NULL;
        uint8_t name_length = 0;
        for (int i = 0; i < parameter->disc_res.num_prop; ++i) {
            esp_bt_gap_dev_prop_t *property = &parameter->disc_res.prop[i];
            if (property->type == ESP_BT_GAP_DEV_PROP_COD) {
                memcpy(&cod_value, property->val, sizeof(cod_value));
            } else if (property->type == ESP_BT_GAP_DEV_PROP_RSSI) {
                rssi = *(int8_t *)property->val;
            } else if (property->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                name = property->val;
                name_length = property->len > UINT8_MAX ? UINT8_MAX : (uint8_t)property->len;
            } else if (property->type == ESP_BT_GAP_DEV_PROP_EIR && name == NULL) {
                name = esp_bt_gap_resolve_eir_data(property->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_length);
                if (name == NULL) {
                    name = esp_bt_gap_resolve_eir_data(property->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_length);
                }
            }
        }
        esp_hid_usage_t usage = esp_hid_usage_from_cod(cod_value);
        bluetooth_hid__usage_t mapped_usage = bluetooth_hid__map_usage(usage);
        if (mapped_usage == BRUCE_BLUETOOTH_HID_UNKNOWN) return;

        size_t index = s_scan_count;
        for (size_t i = 0; i < s_scan_count; ++i) {
            if (memcmp(s_scan_devices[i].address, parameter->disc_res.bda, ESP_BD_ADDR_LEN) == 0) index = i;
        }
        if (index == s_scan_count) {
            if (s_scan_count >= s_scan_capacity) return;
            memset(&s_scan_devices[index], 0, sizeof(s_scan_devices[index]));
            memcpy(s_scan_devices[index].address, parameter->disc_res.bda, ESP_BD_ADDR_LEN);
            s_scan_count++;
        }
        s_scan_devices[index].rssi = rssi;
        s_scan_devices[index].usage = mapped_usage;
        if (name != NULL) bluetooth_hid__copy_name(s_scan_devices[index].name,
                                                   sizeof(s_scan_devices[index].name), name, name_length);
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT &&
               parameter->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        xEventGroupSetBits(s_events, BLUETOOTH_HID__SCAN_DONE_BIT);
    } else if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        (void)esp_bt_gap_ssp_confirm_reply(parameter->cfm_req.bda, true);
    } else if (event == ESP_BT_GAP_KEY_NOTIF_EVT) {
        char message[BRUCE_NOTIFICATION_TEXT_MAX];
        snprintf(message, sizeof(message), "Bluetooth pairing code: %06lu",
                 (unsigned long)parameter->key_notif.passkey);
        (void)notification__push(message, BRUCE_NOTIFICATION_DURATION_MAX_MS);
    } else if (event == ESP_BT_GAP_PIN_REQ_EVT) {
        esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
        (void)esp_bt_gap_pin_reply(parameter->pin_req.bda, true, 4, pin);
    }
}

static void bluetooth_hid__event_callback(void *argument, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)argument;
    (void)base;
    esp_hidh_event_data_t *parameter = event_data;
    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_START_EVENT:
        s_open_status = parameter->start.status;
        xEventGroupSetBits(s_events, BLUETOOTH_HID__START_DONE_BIT);
        break;
    case ESP_HIDH_OPEN_EVENT:
        portENTER_CRITICAL(&s_state_mux);
        bluetooth_hid__connection_t *opened = s_pending_connection;
        bool matched_open = opened != NULL && (opened->dev == NULL || opened->dev == parameter->open.dev);
        if (matched_open && parameter->open.status == ESP_OK) {
            opened->dev = parameter->open.dev;
            s_pending_connection = NULL;
            bluetooth_hid__reset_input_state();
        } else if (opened == NULL || opened->dev != parameter->open.dev) {
            opened = NULL;
        }
        portEXIT_CRITICAL(&s_state_mux);
        if (opened != NULL && parameter->open.status == ESP_OK) {
            const uint8_t *address = esp_hidh_dev_bda_get(parameter->open.dev);
            if (address != NULL) memcpy(opened->snapshot.address, address, ESP_BD_ADDR_LEN);
            const char *name = esp_hidh_dev_name_get(parameter->open.dev);
            if (name != NULL) bluetooth_hid__copy_name(opened->snapshot.name, sizeof(opened->snapshot.name),
                                                       (const uint8_t *)name, strlen(name));
            opened->snapshot.usage = bluetooth_hid__map_usage(esp_hidh_dev_usage_get(parameter->open.dev));
            size_t report_count = 0;
            esp_hid_report_item_t *reports = NULL;
            if (esp_hidh_dev_reports_get(parameter->open.dev, &report_count, &reports) == ESP_OK) {
                for (size_t i = 0; i < report_count; ++i) {
                    if (reports[i].report_type != ESP_HID_REPORT_TYPE_INPUT) continue;
                    if (reports[i].usage == ESP_HID_USAGE_KEYBOARD && reports[i].value_len == 8) {
                        opened->keyboard_supported = true;
                        opened->keyboard_map = reports[i].map_index;
                        opened->keyboard_report_id = reports[i].report_id;
                    } else if ((reports[i].usage == ESP_HID_USAGE_GAMEPAD ||
                                reports[i].usage == ESP_HID_USAGE_JOYSTICK) && reports[i].value_len == 7) {
                        opened->gamepad_supported = true;
                        opened->gamepad_map = reports[i].map_index;
                        opened->gamepad_report_id = reports[i].report_id;
                    }
                }
                free(reports);
            }
            portENTER_CRITICAL(&s_state_mux);
            s_connection = opened;
            portEXIT_CRITICAL(&s_state_mux);
        } else if (parameter->open.status == ESP_OK) {
            (void)esp_hidh_dev_close(parameter->open.dev);
        }
        if (matched_open) {
            s_open_status = parameter->open.status;
            xEventGroupSetBits(s_events, BLUETOOTH_HID__OPEN_DONE_BIT);
        }
        break;
    case ESP_HIDH_INPUT_EVENT: {
        bool keyboard = false;
        bool gamepad = false;
        portENTER_CRITICAL(&s_state_mux);
        bluetooth_hid__connection_t *active = s_connection;
        if (active != NULL && active->dev == parameter->input.dev) {
            keyboard = active->keyboard_supported && active->keyboard_map == parameter->input.map_index &&
                       active->keyboard_report_id == parameter->input.report_id;
            gamepad = active->gamepad_supported && active->gamepad_map == parameter->input.map_index &&
                      active->gamepad_report_id == parameter->input.report_id;
        }
        portEXIT_CRITICAL(&s_state_mux);
        if (keyboard) {
            (void)bluetooth_hid__translate_keyboard_report(parameter->input.data, parameter->input.length);
        } else if (gamepad) {
            (void)bluetooth_hid__translate_gamepad_report(parameter->input.data, parameter->input.length);
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        bool waited = false;
        bool was_active = false;
        portENTER_CRITICAL(&s_state_mux);
        if (s_connection != NULL && s_connection->dev == parameter->close.dev) {
            s_connection->dev = NULL;
            s_connection = NULL;
            was_active = true;
        }
        if (s_pending_connection != NULL && s_pending_connection->dev == parameter->close.dev) {
            s_pending_connection->dev = NULL;
            s_pending_connection = NULL;
        }
        if (s_wait_close_dev == parameter->close.dev) {
            s_wait_close_dev = NULL;
            waited = true;
        }
        portEXIT_CRITICAL(&s_state_mux);
        if (was_active || waited) {
            uint8_t keyboard_release[8] = {0};
            uint8_t gamepad_release[7] = {
                (uint8_t)s_gamepad_axes[0], (uint8_t)s_gamepad_axes[1],
                (uint8_t)s_gamepad_axes[2], (uint8_t)s_gamepad_axes[3], 8, 0, 0,
            };
            (void)bluetooth_hid__translate_keyboard_report(keyboard_release, sizeof(keyboard_release));
            if (s_gamepad_initialized) {
                (void)bluetooth_hid__translate_gamepad_report(gamepad_release, sizeof(gamepad_release));
            }
            bluetooth_hid__reset_input_state();
        }
        (void)esp_hidh_dev_free(parameter->close.dev);
        if (waited) xEventGroupSetBits(s_events, BLUETOOTH_HID__CLOSE_DONE_BIT);
        break;
    }
    default:
        break;
    }
}

static bruce_result_t bluetooth_hid__init(void)
{
    bruce_result_t result = bluetooth__stack_init();
    if (result != BRUCE_OK) return result;
    if (s_hidh_initialized) return BRUCE_OK;

    if (s_events == NULL) s_events = xEventGroupCreate();
    if (s_events == NULL) return BRUCE_ERR_NO_MEMORY;
    if (esp_bt_gap_register_callback(bluetooth_hid__gap_callback) != ESP_OK) return BRUCE_ERR_IO;

    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_IO;
    esp_bt_pin_code_t unused_pin = {0};
    (void)esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability));
    (void)esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, unused_pin);
    (void)esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    esp_hidh_config_t config = {
        .callback = bluetooth_hid__event_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    xEventGroupClearBits(s_events, BLUETOOTH_HID__START_DONE_BIT);
    if (esp_hidh_init(&config) != ESP_OK) return BRUCE_ERR_IO;
    EventBits_t bits = xEventGroupWaitBits(s_events, BLUETOOTH_HID__START_DONE_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(5000));
    if ((bits & BLUETOOTH_HID__START_DONE_BIT) == 0) return BRUCE_ERR_TIMEOUT;
    if (s_open_status != ESP_OK) return BRUCE_ERR_IO;
    s_hidh_initialized = true;
    return BRUCE_OK;
}

static int bluetooth_hid__compare_rssi(const void *left, const void *right)
{
    const bluetooth_hid__device_t *a = left;
    const bluetooth_hid__device_t *b = right;
    return (int)b->rssi - (int)a->rssi;
}

static void bluetooth_hid__connection_cleanup(void *context)
{
    bluetooth_hid__connection_t *connection = context;
    if (connection == NULL) return;
    portENTER_CRITICAL(&s_state_mux);
    esp_hidh_dev_t *dev = connection->dev;
    connection->dev = NULL;
    if (s_connection == connection) s_connection = NULL;
    if (s_pending_connection == connection) s_pending_connection = NULL;
    s_wait_close_dev = dev;
    portEXIT_CRITICAL(&s_state_mux);
    if (dev != NULL) {
        xEventGroupClearBits(s_events, BLUETOOTH_HID__CLOSE_DONE_BIT);
        if (esp_hidh_dev_close(dev) == ESP_OK) {
            (void)xEventGroupWaitBits(s_events, BLUETOOTH_HID__CLOSE_DONE_BIT, pdTRUE, pdFALSE,
                                      pdMS_TO_TICKS(5000));
        }
    }
    bluetooth_hid__connection_unref(connection);
}

#endif

bool bluetooth_hid__is_supported(void)
{
#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED
    return true;
#else
    return false;
#endif
}

int bluetooth_hid__scan(bluetooth_hid__device_t *devices, size_t capacity, uint32_t timeout_ms)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_BT);
    if (permission != BRUCE_OK) return permission;
    if (capacity > 0 && devices == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED
    if (timeout_ms == 0) timeout_ms = BLUETOOTH_HID__DEFAULT_TIMEOUT_MS;
    if (timeout_ms > BLUETOOTH_HID__MAX_TIMEOUT_MS) return BRUCE_ERR_INVALID_ARGUMENT;
    bluetooth_hid__operation_lock();
    bruce_result_t initialized = bluetooth_hid__init();
    if (initialized != BRUCE_OK) {
        bluetooth_hid__operation_unlock();
        return initialized;
    }
    if (s_scan_devices == NULL) {
        s_scan_devices = calloc(BLUETOOTH_HID__MAX_RESULTS, sizeof(bluetooth_hid__device_t));
        if (s_scan_devices == NULL) {
            bluetooth_hid__operation_unlock();
            return BRUCE_ERR_NO_MEMORY;
        }
    }
    s_scan_capacity = capacity < BLUETOOTH_HID__MAX_RESULTS ? capacity : BLUETOOTH_HID__MAX_RESULTS;
    s_scan_count = 0;
    memset(s_scan_devices, 0, BLUETOOTH_HID__MAX_RESULTS * sizeof(bluetooth_hid__device_t));
    s_scanning = true;
    xEventGroupClearBits(s_events, BLUETOOTH_HID__SCAN_DONE_BIT);
    uint8_t inquiry_length = (uint8_t)((timeout_ms + 1279) / 1280);
    esp_err_t error = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, inquiry_length, 0);
    if (error != ESP_OK) {
        s_scanning = false;
        bluetooth_hid__operation_unlock();
        return error == ESP_ERR_INVALID_STATE ? BRUCE_ERR_BUSY : BRUCE_ERR_IO;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BLUETOOTH_HID__SCAN_DONE_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms + 2000));
    if ((bits & BLUETOOTH_HID__SCAN_DONE_BIT) == 0) {
        if (esp_bt_gap_cancel_discovery() == ESP_OK) {
            bits = xEventGroupWaitBits(s_events, BLUETOOTH_HID__SCAN_DONE_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        }
    }
    int count = (int)s_scan_count;
    if (devices != NULL && count > 0) memcpy(devices, s_scan_devices, (size_t)count * sizeof(*devices));
    s_scanning = false;
    s_scan_capacity = 0;
    s_scan_count = 0;
    bluetooth_hid__operation_unlock();
    if ((bits & BLUETOOTH_HID__SCAN_DONE_BIT) == 0) return BRUCE_ERR_TIMEOUT;
    if (count > 1) qsort(devices, (size_t)count, sizeof(*devices), bluetooth_hid__compare_rssi);
    return count;
#else
    (void)devices;
    (void)capacity;
    (void)timeout_ms;
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

bruce_result_t bluetooth_hid__connect(const uint8_t address[BRUCE_BLUETOOTH_ADDRESS_LEN], uint32_t timeout_ms)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_BT);
    if (permission != BRUCE_OK) return permission;
    if (address == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED
    if (timeout_ms == 0) timeout_ms = BLUETOOTH_HID__DEFAULT_TIMEOUT_MS;
    if (timeout_ms > BLUETOOTH_HID__MAX_TIMEOUT_MS) return BRUCE_ERR_INVALID_ARGUMENT;
    bluetooth_hid__operation_lock();
    bruce_result_t initialized = bluetooth_hid__init();
    if (initialized != BRUCE_OK) {
        bluetooth_hid__operation_unlock();
        return initialized;
    }
    portENTER_CRITICAL(&s_state_mux);
    bool busy = s_connection != NULL || s_pending_connection != NULL;
    portEXIT_CRITICAL(&s_state_mux);
    if (busy) {
        bluetooth_hid__operation_unlock();
        return BRUCE_ERR_BUSY;
    }
    bluetooth_hid__connection_t *connection = calloc(1, sizeof(*connection));
    if (connection == NULL) {
        bluetooth_hid__operation_unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    connection->owner = task__current_id();
    connection->refs = 1;
    xEventGroupClearBits(s_events, BLUETOOTH_HID__OPEN_DONE_BIT);
    s_open_status = ESP_FAIL;
    portENTER_CRITICAL(&s_state_mux);
    s_pending_connection = connection;
    portEXIT_CRITICAL(&s_state_mux);
    connection->resource = task_registry__resource_register(bluetooth_hid__connection_cleanup, connection);
    if (connection->resource == BRUCE_RESOURCE_ID_INVALID) {
        bluetooth_hid__connection_cleanup(connection);
        bluetooth_hid__operation_unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    esp_hidh_dev_t *opened_dev = esp_hidh_dev_open((uint8_t *)address, ESP_HID_TRANSPORT_BT, 0);
    if (opened_dev == NULL) {
        portENTER_CRITICAL(&s_state_mux);
        if (s_pending_connection == connection) s_pending_connection = NULL;
        portEXIT_CRITICAL(&s_state_mux);
        (void)task_registry__resource_release(connection->resource);
        bluetooth_hid__connection_unref(connection);
        bluetooth_hid__operation_unlock();
        return BRUCE_ERR_IO;
    }
    portENTER_CRITICAL(&s_state_mux);
    if (connection->dev == NULL) connection->dev = opened_dev;
    portEXIT_CRITICAL(&s_state_mux);
    EventBits_t bits = xEventGroupWaitBits(s_events, BLUETOOTH_HID__OPEN_DONE_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & BLUETOOTH_HID__OPEN_DONE_BIT) == 0) {
        (void)task_registry__resource_release(connection->resource);
        bluetooth_hid__connection_cleanup(connection);
        bluetooth_hid__operation_unlock();
        return BRUCE_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_state_mux);
    bool connected = s_connection == connection && connection->dev != NULL;
    portEXIT_CRITICAL(&s_state_mux);
    if (s_open_status != ESP_OK || !connected) {
        (void)task_registry__resource_release(connection->resource);
        bluetooth_hid__connection_cleanup(connection);
        bluetooth_hid__operation_unlock();
        return BRUCE_ERR_IO;
    }
    bluetooth_hid__operation_unlock();
    return BRUCE_OK;
#else
    (void)timeout_ms;
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

bruce_result_t bluetooth_hid__disconnect(void)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_BT);
    if (permission != BRUCE_OK) return permission;
#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED
    bluetooth_hid__operation_lock();
    portENTER_CRITICAL(&s_state_mux);
    bluetooth_hid__connection_t *connection = s_connection;
    esp_hidh_dev_t *dev = connection != NULL ? connection->dev : NULL;
    if (connection != NULL && dev != NULL) {
        connection->refs++;
        connection->dev = NULL;
        s_connection = NULL;
    }
    s_wait_close_dev = dev;
    portEXIT_CRITICAL(&s_state_mux);
    if (connection == NULL || dev == NULL) {
        bluetooth_hid__operation_unlock();
        return BRUCE_OK;
    }
    xEventGroupClearBits(s_events, BLUETOOTH_HID__CLOSE_DONE_BIT);
    esp_err_t error = esp_hidh_dev_close(dev);
    EventBits_t closed = 0;
    if (error == ESP_OK) {
        closed = xEventGroupWaitBits(s_events, BLUETOOTH_HID__CLOSE_DONE_BIT, pdTRUE, pdFALSE,
                                     pdMS_TO_TICKS(5000));
    }
    if (error == ESP_OK && (closed & BLUETOOTH_HID__CLOSE_DONE_BIT) != 0 &&
        connection->owner == task__current_id() &&
        task_registry__resource_release(connection->resource) == BRUCE_OK) {
        bluetooth_hid__connection_unref(connection);
    }
    bluetooth_hid__connection_unref(connection);
    bluetooth_hid__operation_unlock();
    if (error != ESP_OK) return BRUCE_ERR_IO;
    return (closed & BLUETOOTH_HID__CLOSE_DONE_BIT) != 0 ? BRUCE_OK : BRUCE_ERR_TIMEOUT;
#else
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

bool bluetooth_hid__is_connected(void)
{
#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED
    portENTER_CRITICAL(&s_state_mux);
    bool connected = s_connection != NULL && s_connection->dev != NULL;
    portEXIT_CRITICAL(&s_state_mux);
    return connected;
#else
    return false;
#endif
}

bruce_result_t bluetooth_hid__connected_device(bluetooth_hid__device_t *out_device)
{
    if (out_device == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_BT);
    if (permission != BRUCE_OK) return permission;
#if SOC_BT_CLASSIC_SUPPORTED && CONFIG_BT_CLASSIC_ENABLED && CONFIG_BT_HID_HOST_ENABLED
    portENTER_CRITICAL(&s_state_mux);
    if (s_connection == NULL || s_connection->dev == NULL) {
        portEXIT_CRITICAL(&s_state_mux);
        return BRUCE_ERR_NOT_FOUND;
    }
    *out_device = s_connection->snapshot;
    portEXIT_CRITICAL(&s_state_mux);
    return BRUCE_OK;
#else
    return BRUCE_ERR_UNSUPPORTED;
#endif
}
