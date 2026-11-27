#include "core_sdk/device.h"

#include "sdkconfig.h"

#if CONFIG_SOC_USB_OTG_SUPPORTED

#include <string.h>

#include "core/hid/hid_keyboard.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal_charger.h"
#include "usb/usb_host.h"

#define TAG "usb_host"
#define USB_HOST__WAIT_MS 15000
#define USB_HOST__STEP_MS 10
#define USB_HOST__CONTROL_MS 500
#define USB_HOST__STACK 4096
/* HID class: boot keyboard interface, SET_PROTOCOL to the boot protocol. */
#define USB_HOST__HID_CLASS 0x03
#define USB_HOST__HID_BOOT 0x01
#define USB_HOST__HID_KEYBOARD 0x01
#define USB_HOST__HID_SET_PROTOCOL 0x0B

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    uint8_t new_address; /* 0: none waiting to be opened. */
    bool gone;
    uint8_t interface;
    bool claimed;
    uint8_t endpoint;
    usb_transfer_t *in;
    bool in_busy;
    bool control_done;
    hid_keyboard_t keys;
} usb_host__session_t;

static volatile bruce_usb_host_state_t s_state = BRUCE_USB_HOST_OFF;
static volatile bool s_stop;
static usb_phy_handle_t s_console_phy; /* The port given back to the USB serial console. */
static usb_host__session_t s_session;

static void usb_host__pump(usb_host__session_t *session) {
    uint32_t flags = 0;
    (void)usb_host_lib_handle_events(pdMS_TO_TICKS(USB_HOST__STEP_MS), &flags);
    (void)usb_host_client_handle_events(session->client, pdMS_TO_TICKS(USB_HOST__STEP_MS));
}

static void usb_host__client_event(const usb_host_client_event_msg_t *message, void *arg) {
    usb_host__session_t *session = arg;
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV && session->device == NULL) {
        session->new_address = message->new_dev.address;
    } else if (message->event == USB_HOST_CLIENT_EVENT_DEV_GONE && message->dev_gone.dev_hdl == session->device) {
        session->gone = true;
    }
}

static void usb_host__report(usb_transfer_t *transfer) {
    usb_host__session_t *session = transfer->context;
    session->in_busy = false;
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED || session->gone || s_stop) return;
    if (transfer->actual_num_bytes >= 8) (void)hid_keyboard__report(&session->keys, transfer->data_buffer, 8);
    session->in_busy = usb_host_transfer_submit(transfer) == ESP_OK;
}

static void usb_host__control_done(usb_transfer_t *transfer) {
    ((usb_host__session_t *)transfer->context)->control_done = true;
}

/* A boot keyboard answers in boot reports (8 bytes) only once asked to. */
static bool usb_host__set_boot_protocol(usb_host__session_t *session) {
    usb_transfer_t *control = NULL;
    if (usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE, 0, &control) != ESP_OK) return false;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)control->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = USB_HOST__HID_SET_PROTOCOL;
    setup->wValue = 0;
    setup->wIndex = session->interface;
    setup->wLength = 0;
    control->num_bytes = USB_SETUP_PACKET_SIZE;
    control->device_handle = session->device;
    control->bEndpointAddress = 0;
    control->callback = usb_host__control_done;
    control->context = session;
    session->control_done = false;
    bool sent = usb_host_transfer_submit_control(session->client, control) == ESP_OK;
    for (int waited = 0; sent && !session->control_done && waited < USB_HOST__CONTROL_MS; waited += 2 * USB_HOST__STEP_MS) {
        usb_host__pump(session);
    }
    bool ok = sent && session->control_done && control->status == USB_TRANSFER_STATUS_COMPLETED;
    if (session->control_done || !sent) (void)usb_host_transfer_free(control);
    return ok;
}

/* The device's boot keyboard interface and its interrupt IN endpoint. */
static const usb_ep_desc_t *usb_host__find_keyboard(const usb_config_desc_t *config, uint8_t *out_interface) {
    int offset = 0;
    const usb_standard_desc_t *descriptor = (const usb_standard_desc_t *)config;
    while ((descriptor = usb_parse_next_descriptor_of_type(descriptor, config->wTotalLength,
                                                           USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
        const usb_intf_desc_t *interface = (const usb_intf_desc_t *)descriptor;
        if (interface->bInterfaceClass != USB_HOST__HID_CLASS || interface->bInterfaceSubClass != USB_HOST__HID_BOOT ||
            interface->bInterfaceProtocol != USB_HOST__HID_KEYBOARD) {
            continue;
        }
        for (int i = 0; i < interface->bNumEndpoints; ++i) {
            int endpoint_offset = offset;
            const usb_ep_desc_t *endpoint =
                usb_parse_endpoint_descriptor_by_index(interface, i, config->wTotalLength, &endpoint_offset);
            if (endpoint != NULL && USB_EP_DESC_GET_EP_DIR(endpoint) &&
                USB_EP_DESC_GET_XFERTYPE(endpoint) == USB_TRANSFER_TYPE_INTR) {
                *out_interface = interface->bInterfaceNumber;
                return endpoint;
            }
        }
    }
    return NULL;
}

static void usb_host__close(usb_host__session_t *session) {
    if (session->device == NULL) return;
    if (session->in != NULL) {
        if (session->in_busy) {
            (void)usb_host_endpoint_halt(session->device, session->endpoint);
            (void)usb_host_endpoint_flush(session->device, session->endpoint);
            for (int waited = 0; session->in_busy && waited < USB_HOST__CONTROL_MS; waited += 2 * USB_HOST__STEP_MS) {
                usb_host__pump(session);
            }
            (void)usb_host_endpoint_clear(session->device, session->endpoint);
        }
        if (!session->in_busy) (void)usb_host_transfer_free(session->in);
        session->in = NULL;
    }
    if (session->claimed) (void)usb_host_interface_release(session->client, session->device, session->interface);
    session->claimed = false;
    (void)usb_host_device_close(session->client, session->device);
    session->device = NULL;
    hid_keyboard__release_all(&session->keys);
}

/* Opens a new device; keeps it only if it is a boot keyboard. */
static void usb_host__open(usb_host__session_t *session, uint8_t address) {
    if (usb_host_device_open(session->client, address, &session->device) != ESP_OK) {
        session->device = NULL;
        return;
    }
    const usb_config_desc_t *config = NULL;
    const usb_ep_desc_t *endpoint = NULL;
    if (usb_host_get_active_config_descriptor(session->device, &config) == ESP_OK) {
        endpoint = usb_host__find_keyboard(config, &session->interface);
    }
    if (endpoint == NULL) {
        ESP_LOGW(TAG, "device %u is not a keyboard", address);
        usb_host__close(session);
        return;
    }
    session->endpoint = endpoint->bEndpointAddress;
    int size = USB_EP_DESC_GET_MPS(endpoint);
    session->claimed = usb_host_interface_claim(session->client, session->device, session->interface, 0) == ESP_OK;
    if (!session->claimed || !usb_host__set_boot_protocol(session) ||
        usb_host_transfer_alloc(size, 0, &session->in) != ESP_OK) {
        ESP_LOGW(TAG, "keyboard %u did not start", address);
        usb_host__close(session);
        return;
    }
    session->in->num_bytes = size;
    session->in->device_handle = session->device;
    session->in->bEndpointAddress = session->endpoint;
    session->in->callback = usb_host__report;
    session->in->context = session;
    session->in_busy = usb_host_transfer_submit(session->in) == ESP_OK;
    if (!session->in_busy) {
        usb_host__close(session);
        return;
    }
    s_state = BRUCE_USB_HOST_CONNECTED;
    ESP_LOGI(TAG, "keyboard connected");
}

/* Until the device is unplugged, none comes within USB_HOST__WAIT_MS, or a stop.
 * Boot keyboards are the only devices taken so far. */
static void usb_host__serve(usb_host__session_t *session) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(USB_HOST__WAIT_MS);
    while (!s_stop && !session->gone) {
        usb_host__pump(session);
        if (session->new_address != 0) {
            uint8_t address = session->new_address;
            session->new_address = 0;
            usb_host__open(session, address);
        }
        if (session->device == NULL && (int32_t)(xTaskGetTickCount() - deadline) >= 0) break;
    }
}

static void usb_host__task(void *context) {
    const hal_device_t *charger = context;
    usb_host__session_t *session = &s_session;
    memset(session, 0, sizeof(*session));
    if (s_console_phy != NULL && usb_del_phy(s_console_phy) == ESP_OK) s_console_phy = NULL;
    const usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    if (usb_host_install(&host_config) == ESP_OK) {
        const usb_host_client_config_t client_config = {
            .max_num_event_msg = 4,
            .async = {.client_event_callback = usb_host__client_event, .callback_arg = session},
        };
        if (usb_host_client_register(&client_config, &session->client) == ESP_OK) {
            usb_host__serve(session);
            usb_host__close(session);
            (void)usb_host_client_deregister(session->client);
        }
        if (usb_host_device_free_all() == ESP_ERR_NOT_FINISHED) {
            for (int waited = 0; waited < 1000; waited += USB_HOST__STEP_MS) {
                uint32_t flags = 0;
                (void)usb_host_lib_handle_events(pdMS_TO_TICKS(USB_HOST__STEP_MS), &flags);
                if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) break;
            }
        }
        if (usb_host_uninstall() != ESP_OK) ESP_LOGE(TAG, "USB host did not uninstall");
    } else {
        ESP_LOGE(TAG, "USB host did not install");
    }
    /* The port back to the USB serial console. */
    const usb_phy_config_t console = {.controller = USB_PHY_CTRL_SERIAL_JTAG, .target = USB_PHY_TARGET_INT};
    if (usb_new_phy(&console, &s_console_phy) != ESP_OK) ESP_LOGE(TAG, "USB console did not come back");
    (void)hal_charger_set_otg(charger, false);
    ESP_LOGI(TAG, "USB port back to the console");
    s_state = BRUCE_USB_HOST_OFF;
    vTaskDelete(NULL);
}

bruce_result_t device__usb_host_start(void) {
    const hal_device_t *charger = hal_device_first(HAL_CLASS_CHARGER);
    if (charger == NULL) return BRUCE_ERR_UNSUPPORTED;
    if (s_state != BRUCE_USB_HOST_OFF) return BRUCE_OK;
    bruce_device_power_t power;
    bruce_result_t result = hal_charger_read(charger, false, &power);
    if (result != BRUCE_OK) return result;
    /* Never 5 V against a computer or charger: the chip wouldn't notice them. */
    if (power.input != BRUCE_POWER_INPUT_NONE) return BRUCE_ERR_BUSY;
    result = hal_charger_set_otg(charger, true);
    if (result != BRUCE_OK) return result;
    s_stop = false;
    s_state = BRUCE_USB_HOST_WAITING;
    if (xTaskCreate(usb_host__task, "usb_host", USB_HOST__STACK, (void *)charger, 5, NULL) != pdPASS) {
        (void)hal_charger_set_otg(charger, false);
        s_state = BRUCE_USB_HOST_OFF;
        return BRUCE_ERR_NO_MEMORY;
    }
    ESP_LOGI(TAG, "USB port powered, waiting for a device");
    return BRUCE_OK;
}

bruce_result_t device__usb_host_stop(void) {
    if (s_state != BRUCE_USB_HOST_OFF) s_stop = true;
    return BRUCE_OK;
}

bruce_usb_host_state_t device__usb_host_state(void) { return s_state; }

#else

bruce_result_t device__usb_host_start(void) { return BRUCE_ERR_UNSUPPORTED; }

bruce_result_t device__usb_host_stop(void) { return BRUCE_OK; }

bruce_usb_host_state_t device__usb_host_state(void) { return BRUCE_USB_HOST_OFF; }

#endif
