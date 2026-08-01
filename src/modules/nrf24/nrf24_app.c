#include "nrf24_app.h"

#include <stdio.h>
#include <stdlib.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/nrf24.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

#define NRF24_APP_SPECTRUM_CHANNELS 80u
#define NRF24_APP_SPECTRUM_SAMPLES 8u

/* Renders NRF24's GUI menu inside the launcher's window chrome (same border,
 * status bar, and font as the WiFi submenu); a no-op outside GUI mode. */
static const bruce_dialog_render_params_t s_window_chrome = {.window_chrome = true};

static bool nrf24_app__parse_channel(const char *text, uint8_t *out) {
    if (text == NULL || out == NULL) return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > BRUCE_NRF24_CHANNEL_MAX) return false;
    *out = (uint8_t)value;
    return true;
}

static int nrf24_app__status(bool gui) {
    bool connected = false;
    bruce_result_t result = nrf24__probe(&connected);
    bruce_nrf24_pins_t pins;
    nrf24__get_pins(&pins);
    char message[160];
    snprintf(
        message,
        sizeof(message),
        "Radio: %s\nSPI3 SCK:%d MISO:%d MOSI:%d\nCS:%d CE:%d",
        result == BRUCE_OK && connected ? "connected" : "not found",
        pins.sck,
        pins.miso,
        pins.mosi,
        pins.cs,
        pins.ce
    );
    if (gui)
        (void)dialog__message(
            result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_WARNING, "NRF24 status", message
        );
    else stdio__printf("%s\n", message);
    return result;
}

static int nrf24_app__scan(uint8_t first, uint8_t last, uint8_t samples, bool gui) {
    size_t count = (size_t)last - first + 1u;
    uint8_t activity[BRUCE_NRF24_CHANNEL_MAX + 1u];
    bruce_result_t result = nrf24__scan(first, count, samples, activity);
    if (result != BRUCE_OK) {
        if (gui)
            (void)dialog__message(BRUCE_DIALOG_ERROR, "NRF24 spectrum", "Scan failed or radio not found");
        else stdio__printf("NRF24 scan failed: %d\n", result);
        return result;
    }

    if (!gui) {
        for (size_t index = 0; index < count; ++index) {
            stdio__printf("%3u %3u/%u ", (unsigned int)(first + index), activity[index], samples);
            for (uint8_t bar = 0; bar < activity[index]; ++bar) (void)stdio__write("#", 1);
            (void)stdio__write("\n", 1);
        }
        return BRUCE_OK;
    }

    char spectrum[768];
    size_t used =
        (size_t)snprintf(spectrum, sizeof(spectrum), "2.4 GHz RPD activity (%u samples)\n\n", samples);
    for (size_t row = 0; row < count && used + 32 < sizeof(spectrum); row += 10) {
        int written = snprintf(
            spectrum + used,
            sizeof(spectrum) - used,
            "%02u-%02u  ",
            (unsigned int)(first + row),
            (unsigned int)(first + (row + 9 < count ? row + 9 : count - 1))
        );
        if (written < 0 || (size_t)written >= sizeof(spectrum) - used) break;
        used += (size_t)written;
        for (size_t index = row; index < row + 10 && index < count && used + 2 < sizeof(spectrum); ++index) {
            static const char levels[] = " .:-=+*#%@";
            size_t level = ((size_t)activity[index] * 9u + samples - 1u) / samples;
            spectrum[used++] = levels[level];
        }
        spectrum[used++] = '\n';
        spectrum[used] = '\0';
    }
    (void)dialog__message(BRUCE_DIALOG_INFO, "NRF24 spectrum", spectrum);
    return BRUCE_OK;
}

static int nrf24_app__gui(void) {
    const bruce_dialog_choice_t choices[] = {
        {.label = "Spectrum scan", .value = "scan"  },
        {.label = "Radio status",  .value = "status"},
        {.label = "Information",   .value = "info"  },
        {.label = "Exit",          .value = "exit"  },
    };
    for (;;) {
        size_t selected = 0;
        bruce_result_t result =
            dialog__choice("NRF24", "2.4 GHz radio tools", choices, 4, &selected, &s_window_chrome);
        if (result == BRUCE_ERR_CANCELLED || selected == 3) return 0;
        if (result != BRUCE_OK) return result;
        if (selected == 0)
            (void)nrf24_app__scan(0, NRF24_APP_SPECTRUM_CHANNELS - 1u, NRF24_APP_SPECTRUM_SAMPLES, true);
        else if (selected == 1) (void)nrf24_app__status(true);
        else
            (void)dialog__message(
                BRUCE_DIALOG_INFO,
                "NRF24",
                "Passive spectrum scan shows threshold activity, not RSSI or decoded packets.\n\n"
                "Use short wiring and a stable 3.3 V supply. PA/LNA radios should have local decoupling."
            );
    }
}

int nrf24_app_main(int argc, char **argv) {
    if (app_runner__args_have_gui(argc, argv)) {
        if (!app_runner__args_have_background(argc, argv)) {
            bruce_result_t foreground = process__to_foreground();
            if (foreground != BRUCE_OK) return foreground;
        }
        return nrf24_app__gui();
    }

    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Inspect and configure an NRF24 2.4 GHz radio.");
    ArgParser *status = ap_new_cmd(root, "status");
    ArgParser *channel_command = ap_new_cmd(root, "channel");
    ArgParser *scan = ap_new_cmd(root, "scan");
    ap_set_helptext(status, "Show radio and pin status.");
    ap_set_helptext(channel_command, "Set the active NRF24 channel.");
    ap_add_required_arg(channel_command, "channel", "Radio channel (0-125)");
    ap_set_helptext(scan, "Scan a channel range for RPD activity.");
    ap_add_optional_arg(scan, "first", "First channel (default 0)");
    ap_add_optional_arg(scan, "last", "Last channel (default 79)");
    ap_add_optional_arg(scan, "samples", "Samples per channel (default 8, maximum 255)");
    if (!ap_parse(root, argc, argv)) {
        ap_status_t parse_status = ap_get_status(root);
        if (parse_status != AP_STATUS_HELP && parse_status != AP_STATUS_VERSION)
            ap_print_help(ap_get_cmd_parser(root) != NULL ? ap_get_cmd_parser(root) : root);
        int result = parse_status == AP_STATUS_HELP || parse_status == AP_STATUS_VERSION ? BRUCE_OK
                     : parse_status == AP_STATUS_NO_MEMORY                               ? BRUCE_ERR_NO_MEMORY
                                                           : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }

    int result;
    ArgParser *command = ap_get_cmd_parser(root);
    if (command == NULL || command == status) {
        result = nrf24_app__status(false);
    } else if (command == channel_command) {
        uint8_t channel = 0;
        if (!nrf24_app__parse_channel(ap_get_arg(channel_command, "channel"), &channel)) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else {
            result = nrf24__set_channel(channel);
            if (result == BRUCE_OK) stdio__printf("NRF24 channel set to %u\n", channel);
            else stdio__printf("NRF24 channel failed: %d\n", result);
        }
    } else {
        uint8_t first = 0;
        uint8_t last = NRF24_APP_SPECTRUM_CHANNELS - 1u;
        uint8_t samples = NRF24_APP_SPECTRUM_SAMPLES;
        const char *first_text = ap_get_arg(scan, "first");
        const char *last_text = ap_get_arg(scan, "last");
        const char *samples_text = ap_get_arg(scan, "samples");
        if ((first_text != NULL && !nrf24_app__parse_channel(first_text, &first)) ||
            (last_text != NULL && !nrf24_app__parse_channel(last_text, &last)) || last < first) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
        } else if (samples_text != NULL) {
            char *end = NULL;
            unsigned long value = strtoul(samples_text, &end, 10);
            if (end == samples_text || *end != '\0' || value == 0 || value > UINT8_MAX)
                result = BRUCE_ERR_INVALID_ARGUMENT;
            else {
                samples = (uint8_t)value;
                result = nrf24_app__scan(first, last, samples, false);
            }
        } else {
            result = nrf24_app__scan(first, last, samples, false);
        }
    }
    ap_free(root);
    return result;
}
