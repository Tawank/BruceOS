#include "nrf24_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/nrf24.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

#define NRF24_APP_SPECTRUM_CHANNELS 80u
#define NRF24_APP_SPECTRUM_SAMPLES 8u

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
            for (uint8_t bar = 0; bar < activity[index]; ++bar) (void)bruce_stdio_write("#", 1);
            (void)bruce_stdio_write("\n", 1);
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
        bruce_result_t result = dialog__choice("NRF24", "2.4 GHz radio tools", choices, 4, &selected, NULL);
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

static void nrf24_app__usage(void) {
    stdio__printf("NRF24 commands:\n");
    stdio__printf("  nrf24 status\n");
    stdio__printf("  nrf24 channel <0-125>\n");
    stdio__printf("  nrf24 scan [first] [last] [samples]\n");
}

int nrf24_app_main(int argc, char **argv) {
    if (app_runner__args_have_gui(argc, argv)) return nrf24_app__gui();
    if (argc <= 1 || argv == NULL || argv[1] == NULL || strcmp(argv[1], "status") == 0) {
        return nrf24_app__status(false);
    }
    if (strcmp(argv[1], "channel") == 0) {
        uint8_t channel = 0;
        if (argc != 3 || !nrf24_app__parse_channel(argv[2], &channel)) return BRUCE_ERR_INVALID_ARGUMENT;
        bruce_result_t result = nrf24__set_channel(channel);
        if (result == BRUCE_OK) stdio__printf("NRF24 channel set to %u\n", channel);
        else stdio__printf("NRF24 channel failed: %d\n", result);
        return result;
    }
    if (strcmp(argv[1], "scan") == 0) {
        uint8_t first = 0;
        uint8_t last = NRF24_APP_SPECTRUM_CHANNELS - 1u;
        uint8_t samples = NRF24_APP_SPECTRUM_SAMPLES;
        if ((argc > 2 && !nrf24_app__parse_channel(argv[2], &first)) ||
            (argc > 3 && !nrf24_app__parse_channel(argv[3], &last)) || last < first) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        if (argc > 4) {
            char *end = NULL;
            unsigned long value = strtoul(argv[4], &end, 10);
            if (end == argv[4] || *end != '\0' || value == 0 || value > UINT8_MAX) {
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
            samples = (uint8_t)value;
        }
        return nrf24_app__scan(first, last, samples, false);
    }
    nrf24_app__usage();
    return strcmp(argv[1], "help") == 0 ? BRUCE_OK : BRUCE_ERR_INVALID_ARGUMENT;
}
