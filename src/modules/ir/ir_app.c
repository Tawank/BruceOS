#include "ir_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/ir.h"
#include "core_sdk/result.h"
#include "core_sdk/storage.h"

#define IR_APP_CAPTURE_SIZE 8192u

static bool ir_app__parse_u32(const char *text, uint32_t maximum, uint32_t *out)
{
    if (text == NULL || out == NULL) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > maximum) return false;
    *out = (uint32_t)parsed;
    return true;
}

static void ir_app__usage(void)
{
    printf("IR commands:\n");
    printf("  ir rx [raw] [timeout_seconds]\n");
    printf("  ir tx <protocol> <hex_data> [bits] [repeats]\n");
    printf("  ir tx <protocol> <address> <command>\n");
    printf("  ir tx_raw <frequency_hz> <mark_us> <space_us> [...]\n");
    printf("  ir tx_from_file <absolute_path> [repeats]\n");
}

static int ir_app__receive(bool raw, uint32_t timeout_ms, bool show_dialog)
{
    char *capture = malloc(IR_APP_CAPTURE_SIZE);
    if (capture == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = ir__receive(raw, timeout_ms, capture, IR_APP_CAPTURE_SIZE);
    if (result == BRUCE_OK) {
        if (show_dialog) (void)dialog__message(BRUCE_DIALOG_INFO, "IR capture", capture);
        else printf("%s", capture);
    } else if (result == BRUCE_ERR_TIMEOUT) {
        printf("IR receive timed out\n");
    } else if (result == BRUCE_ERR_UNSUPPORTED) {
        printf("IR decoding failed; retry with raw mode\n");
    } else {
        printf("IR receive failed: %d\n", result);
    }
    free(capture);
    return result;
}

static int ir_app__gui(void)
{
    char message[80];
    snprintf(message, sizeof(message), "TX GPIO %d, RX GPIO %d", ir__tx_pin(), ir__rx_pin());
    const bruce_dialog_choice_t choices[] = {
        {.label = "Read signal", .value = "read"},
        {.label = "Read raw signal", .value = "raw"},
        {.label = "Transmit .ir file", .value = "file"},
        {.label = "Exit", .value = "exit"},
    };
    for (;;) {
        size_t selected = 0;
        bruce_result_t result = dialog__choice("Infrared", message, choices, 4, &selected);
        if (result == BRUCE_ERR_CANCELLED || selected == 3) return 0;
        if (result != BRUCE_OK) return result;
        if (selected < 2) {
            (void)ir_app__receive(selected == 1, 10000, true);
            continue;
        }
        char path[BRUCE_STORAGE_PATH_MAX];
        result = dialog__pick_file("/", ".ir", path, sizeof(path));
        if (result == BRUCE_ERR_CANCELLED) continue;
        if (result == BRUCE_OK) result = ir__transmit_file(path, 0);
        (void)dialog__message(result == BRUCE_OK ? BRUCE_DIALOG_SUCCESS : BRUCE_DIALOG_ERROR,
                              "Infrared", result == BRUCE_OK ? "Transmission complete" : "Transmission failed");
    }
}

static int ir_app__rx(int argc, char **argv)
{
    bool raw = false;
    uint32_t timeout_seconds = 10;
    int index = 1;
    if (argc > index && strcmp(argv[index], "raw") == 0) {
        raw = true;
        index++;
    }
    if (argc > index) {
        uint32_t parsed = 0;
        if (!ir_app__parse_u32(argv[index], UINT32_MAX / 1000u, &parsed) || parsed == 0) {
            printf("Invalid receive timeout\n");
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        timeout_seconds = parsed;
    }
    printf("Waiting for IR signal...\n");
    return ir_app__receive(raw, timeout_seconds * 1000u, false);
}

static int ir_app__tx(int argc, char **argv)
{
    if (argc < 3) {
        ir_app__usage();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (argc == 4 && strlen(argv[2]) == 8 && strlen(argv[3]) == 8) {
        bruce_result_t parsed_result = ir__transmit_parsed(argv[1], argv[2], argv[3], 0);
        printf(parsed_result == BRUCE_OK ? "IR command sent\n" : "IR transmit failed: %d\n", parsed_result);
        return parsed_result;
    }
    uint32_t bits_value = 32;
    uint32_t repeats_value = 0;
    if ((argc > 3 && !ir_app__parse_u32(argv[3], 32, &bits_value)) || bits_value == 0 ||
        (argc > 4 && !ir_app__parse_u32(argv[4], UINT8_MAX, &repeats_value))) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    uint8_t bits = (uint8_t)bits_value;
    uint8_t repeats = (uint8_t)repeats_value;
    bruce_result_t result = ir__transmit(argv[2], argv[1], bits, repeats);
    printf(result == BRUCE_OK ? "IR command sent\n" : "IR transmit failed: %d\n", result);
    return result;
}

static int ir_app__tx_raw(int argc, char **argv)
{
    if (argc < 3) {
        ir_app__usage();
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    uint32_t frequency = 0;
    if (!ir_app__parse_u32(argv[1], 100000, &frequency)) return BRUCE_ERR_INVALID_ARGUMENT;
    uint32_t *timings = malloc(BRUCE_IR_MAX_RAW_TIMINGS * sizeof(*timings));
    if (timings == NULL) return BRUCE_ERR_NO_MEMORY;
    size_t count = 0;
    for (int arg = 2; arg < argc; ++arg) {
        const char *cursor = argv[arg];
        while (*cursor != '\0') {
            while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
            if (*cursor == '\0') break;
            char *end = NULL;
            unsigned long duration = strtoul(cursor, &end, 10);
            if (end == cursor || duration == 0 || duration > 32767 || count >= BRUCE_IR_MAX_RAW_TIMINGS) {
                free(timings);
                return count >= BRUCE_IR_MAX_RAW_TIMINGS ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_ERR_INVALID_ARGUMENT;
            }
            timings[count++] = (uint32_t)duration;
            cursor = end;
        }
    }
    bruce_result_t result = ir__transmit_raw(timings, count, frequency, 0);
    free(timings);
    printf(result == BRUCE_OK ? "Raw IR command sent\n" : "Raw IR transmit failed: %d\n", result);
    return result;
}

int ir_app_main(int argc, char **argv)
{
    if (app_runner__args_have_gui(argc, argv)) return ir_app__gui();
    if (argc == 0 || argv == NULL || argv[0] == NULL || strcmp(argv[0], "help") == 0) {
        ir_app__usage();
        return 0;
    }
    if (strcmp(argv[0], "rx") == 0) return ir_app__rx(argc, argv);
    if (strcmp(argv[0], "tx") == 0) return ir_app__tx(argc, argv);
    if (strcmp(argv[0], "tx_raw") == 0) return ir_app__tx_raw(argc, argv);
    if (strcmp(argv[0], "tx_from_file") == 0) {
        if (argc < 2) return BRUCE_ERR_INVALID_ARGUMENT;
        uint32_t repeats_value = 0;
        if (argc > 2 && !ir_app__parse_u32(argv[2], UINT8_MAX, &repeats_value)) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        uint8_t repeats = (uint8_t)repeats_value;
        bruce_result_t result = ir__transmit_file(argv[1], repeats);
        printf(result == BRUCE_OK ? "IR file sent\n" : "IR file transmit failed: %d\n", result);
        return result;
    }
    ir_app__usage();
    return BRUCE_ERR_INVALID_ARGUMENT;
}
