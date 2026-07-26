#include "ir.h"

#include "core_sdk/ir.h"
#include "core_sdk/permission.h"
#include "core_sdk/storage.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define IR_RESOLUTION_HZ 1000000u
#define IR_RX_SYMBOLS ((BRUCE_IR_MAX_RAW_TIMINGS + 1u) / 2u)
#define IR_FILE_MAX_SIZE (64u * 1024u)
#define IR_TX_TIMEOUT_MS 5000

static SemaphoreHandle_t s_ir_mutex;

bruce_result_t ir__init(void)
{
    if (s_ir_mutex != NULL) return BRUCE_OK;
    s_ir_mutex = xSemaphoreCreateMutex();
    return s_ir_mutex != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
}

int ir__tx_pin(void)
{
    return CONFIG_BRUCE_IR_TX_GPIO;
}

int ir__rx_pin(void)
{
    return CONFIG_BRUCE_IR_RX_GPIO;
}

static bruce_result_t ir__esp_result(esp_err_t error)
{
    if (error == ESP_OK) return BRUCE_OK;
    if (error == ESP_ERR_INVALID_ARG) return BRUCE_ERR_INVALID_ARGUMENT;
    if (error == ESP_ERR_NO_MEM) return BRUCE_ERR_NO_MEMORY;
    if (error == ESP_ERR_NOT_FOUND) return BRUCE_ERR_BUSY;
    if (error == ESP_ERR_INVALID_STATE) return BRUCE_ERR_INVALID_STATE;
    if (error == ESP_ERR_TIMEOUT) return BRUCE_ERR_TIMEOUT;
    if (error == ESP_ERR_NOT_SUPPORTED) return BRUCE_ERR_UNSUPPORTED;
    return BRUCE_ERR_IO;
}

static bool ir__lock(void)
{
    return s_ir_mutex != NULL && xSemaphoreTake(s_ir_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void ir__unlock(void)
{
    xSemaphoreGive(s_ir_mutex);
}

static bruce_result_t ir__send_symbols(const rmt_symbol_word_t *symbols, size_t symbol_count,
                                       uint32_t frequency_hz, uint8_t repeats)
{
    rmt_channel_handle_t channel = NULL;
    rmt_encoder_handle_t encoder = NULL;
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = CONFIG_BRUCE_IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    esp_err_t error = rmt_new_tx_channel(&channel_config, &channel);
    if (error != ESP_OK) return ir__esp_result(error);

    rmt_copy_encoder_config_t encoder_config = {};
    error = rmt_new_copy_encoder(&encoder_config, &encoder);
    if (error == ESP_OK) {
        rmt_carrier_config_t carrier = {
            .frequency_hz = frequency_hz,
            .duty_cycle = 0.33f,
        };
        error = rmt_apply_carrier(channel, &carrier);
    }
    if (error == ESP_OK) error = rmt_enable(channel);

    rmt_transmit_config_t transmit_config = {.loop_count = 0};
    for (uint16_t send = 0; error == ESP_OK && send <= repeats; ++send) {
        error = rmt_transmit(channel, encoder, symbols, symbol_count * sizeof(*symbols), &transmit_config);
        if (error == ESP_OK) error = rmt_tx_wait_all_done(channel, IR_TX_TIMEOUT_MS);
    }

    if (channel != NULL) {
        (void)rmt_disable(channel);
        (void)rmt_del_channel(channel);
    }
    if (encoder != NULL) (void)rmt_del_encoder(encoder);
    gpio_set_level(CONFIG_BRUCE_IR_TX_GPIO, 0);
    return ir__esp_result(error);
}

bruce_result_t ir__transmit_raw(const uint32_t *timings_us, size_t timing_count,
                                uint32_t frequency_hz, uint8_t repeats)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_IR);
    if (permission != BRUCE_OK) return permission;
    if (timings_us == NULL || timing_count < 2 || timing_count > BRUCE_IR_MAX_RAW_TIMINGS ||
        frequency_hz < 20000 || frequency_hz > 100000) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (!ir__lock()) return BRUCE_ERR_BUSY;

    size_t symbol_count = (timing_count + 1u) / 2u;
    rmt_symbol_word_t *symbols = calloc(symbol_count, sizeof(*symbols));
    if (symbols == NULL) {
        ir__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < timing_count; ++i) {
        uint32_t duration = timings_us[i];
        if (duration == 0 || duration > 32767) {
            free(symbols);
            ir__unlock();
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        if ((i & 1u) == 0) {
            symbols[i / 2u].level0 = 1;
            symbols[i / 2u].duration0 = duration;
        } else {
            symbols[i / 2u].level1 = 0;
            symbols[i / 2u].duration1 = duration;
        }
    }

    bruce_result_t result = ir__send_symbols(symbols, symbol_count, frequency_hz, repeats);
    free(symbols);
    ir__unlock();
    return result;
}

static bool ir__append_pair(rmt_symbol_word_t *symbols, size_t capacity, size_t *count,
                            uint16_t mark, uint16_t space)
{
    if (*count >= capacity) return false;
    symbols[*count] = (rmt_symbol_word_t){
        .level0 = 1,
        .duration0 = mark,
        .level1 = 0,
        .duration1 = space,
    };
    (*count)++;
    return true;
}

static bruce_result_t ir__encode_nec(uint32_t data, uint8_t bits, uint16_t leader_mark,
                                     uint16_t leader_space, rmt_symbol_word_t *symbols,
                                     size_t capacity, size_t *out_count)
{
    if (bits == 0 || bits > 32 ||
        !ir__append_pair(symbols, capacity, out_count, leader_mark, leader_space)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    for (int bit = bits - 1; bit >= 0; --bit) {
        if (!ir__append_pair(symbols, capacity, out_count, 560, (data & (1u << bit)) ? 1690 : 560)) {
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
    }
    return ir__append_pair(symbols, capacity, out_count, 560, 1) ? BRUCE_OK : BRUCE_ERR_RESOURCE_LIMIT;
}

static bruce_result_t ir__encode_sony_frame(uint32_t data, uint8_t bits, rmt_symbol_word_t *symbols,
                                            size_t capacity, size_t *out_count)
{
    if (bits == 0 || bits > 32 || !ir__append_pair(symbols, capacity, out_count, 2400, 600)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    for (uint8_t bit = 0; bit < bits; ++bit) {
        if (!ir__append_pair(symbols, capacity, out_count, (data & (1u << bit)) ? 1200 : 600, 600)) {
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
    }
    uint32_t frame_duration = 3000;
    for (uint8_t bit = 0; bit < bits; ++bit) frame_duration += (data & (1u << bit)) ? 1800 : 1200;
    uint32_t gap = frame_duration < 45000 ? 45000 - frame_duration : 600;
    symbols[*out_count - 1].duration1 = (uint16_t)(symbols[*out_count - 1].duration1 + gap);
    return BRUCE_OK;
}

bruce_result_t ir__transmit(const char *data_hex, const char *protocol, uint8_t bits, uint8_t repeats)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_IR);
    if (permission != BRUCE_OK) return permission;
    if (data_hex == NULL || data_hex[0] == '\0' || protocol == NULL || protocol[0] == '\0') {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    char *end = NULL;
    unsigned long long parsed = strtoull(data_hex, &end, 16);
    if (end == data_hex || *end != '\0' || parsed > UINT32_MAX) return BRUCE_ERR_INVALID_ARGUMENT;

    rmt_symbol_word_t symbols[128] = {0};
    size_t symbol_count = 0;
    bruce_result_t result;
    if (strcasecmp(protocol, "NEC") == 0 || strcasecmp(protocol, "NECext") == 0 ||
        strcasecmp(protocol, "Samsung32") == 0) {
        bool samsung = strcasecmp(protocol, "Samsung32") == 0;
        result = ir__encode_nec((uint32_t)parsed, bits, samsung ? 4500 : 9000, 4500,
                                symbols, 128, &symbol_count);
    } else if (strcasecmp(protocol, "SIRC") == 0 || strcasecmp(protocol, "SIRC15") == 0 ||
               strcasecmp(protocol, "SIRC20") == 0 || strcasecmp(protocol, "SONY") == 0) {
        if (strcasecmp(protocol, "SIRC") == 0) bits = 12;
        else if (strcasecmp(protocol, "SIRC15") == 0) bits = 15;
        else if (strcasecmp(protocol, "SIRC20") == 0) bits = 20;
        result = BRUCE_OK;
        for (int frame = 0; frame < 3 && result == BRUCE_OK; ++frame) {
            result = ir__encode_sony_frame((uint32_t)parsed, bits, symbols, 128, &symbol_count);
        }
    } else {
        return BRUCE_ERR_UNSUPPORTED;
    }
    if (result != BRUCE_OK) return result;
    if (!ir__lock()) return BRUCE_ERR_BUSY;
    result = ir__send_symbols(symbols, symbol_count, BRUCE_IR_DEFAULT_FREQUENCY_HZ, repeats);
    ir__unlock();
    return result;
}

typedef struct {
    QueueHandle_t queue;
} ir__rx_context_t;

static bool ir__rx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *event, void *user_data)
{
    (void)channel;
    ir__rx_context_t *context = user_data;
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(context->queue, event, &wake);
    return wake == pdTRUE;
}

static bool ir__duration_near(uint32_t duration, uint32_t expected)
{
    uint32_t tolerance = expected / 4u;
    return duration >= expected - tolerance && duration <= expected + tolerance;
}

static bool ir__decode_nec(const rmt_symbol_word_t *symbols, size_t count, uint32_t *out_data)
{
    if (count < 34 || !ir__duration_near(symbols[0].duration0, 9000) ||
        !ir__duration_near(symbols[0].duration1, 4500)) return false;
    uint32_t data = 0;
    for (size_t i = 0; i < 32; ++i) {
        const rmt_symbol_word_t *symbol = &symbols[i + 1];
        if (!ir__duration_near(symbol->duration0, 560)) return false;
        data <<= 1;
        if (ir__duration_near(symbol->duration1, 1690)) data |= 1u;
        else if (!ir__duration_near(symbol->duration1, 560)) return false;
    }
    *out_data = data;
    return true;
}

bruce_result_t ir__receive(bool raw, uint32_t timeout_ms, char *out, size_t out_size)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_IR);
    if (permission != BRUCE_OK) return permission;
    if (timeout_ms == 0 || out == NULL || out_size == 0) return BRUCE_ERR_INVALID_ARGUMENT;
    out[0] = '\0';
    if (!ir__lock()) return BRUCE_ERR_BUSY;

    rmt_symbol_word_t *symbols = calloc(IR_RX_SYMBOLS, sizeof(*symbols));
    QueueHandle_t queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    rmt_channel_handle_t channel = NULL;
    bruce_result_t result = BRUCE_ERR_NO_MEMORY;
    if (symbols == NULL || queue == NULL) goto cleanup;

    rmt_rx_channel_config_t channel_config = {
        .gpio_num = CONFIG_BRUCE_IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .flags.invert_in = true,
    };
    esp_err_t error = rmt_new_rx_channel(&channel_config, &channel);
    if (error != ESP_OK) {
        result = ir__esp_result(error);
        goto cleanup;
    }
    ir__rx_context_t context = {.queue = queue};
    rmt_rx_event_callbacks_t callbacks = {.on_recv_done = ir__rx_done};
    error = rmt_rx_register_event_callbacks(channel, &callbacks, &context);
    if (error == ESP_OK) error = rmt_enable(channel);
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 3000,
        .signal_range_max_ns = 15000000,
    };
    if (error == ESP_OK) error = rmt_receive(channel, symbols, IR_RX_SYMBOLS * sizeof(*symbols), &receive_config);
    if (error != ESP_OK) {
        result = ir__esp_result(error);
        goto cleanup;
    }

    rmt_rx_done_event_data_t event = {0};
    if (xQueueReceive(queue, &event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        result = BRUCE_ERR_TIMEOUT;
        goto cleanup;
    }
    if (event.num_symbols >= IR_RX_SYMBOLS) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }

    if (!raw) {
        uint32_t data = 0;
        if (!ir__decode_nec(event.received_symbols, event.num_symbols, &data)) {
            result = BRUCE_ERR_UNSUPPORTED;
            goto cleanup;
        }
        int written = snprintf(out, out_size,
            "Filetype: IR signals file\nVersion: 1\n#\n#\nname: Unknown\n"
            "type: parsed\nprotocol: NEC\nbits: 32\nvalue: %08" PRIX32 "\n#\n", data);
        result = written >= 0 && (size_t)written < out_size ? BRUCE_OK : BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }

    size_t used = 0;
    int written = snprintf(out, out_size,
        "Filetype: IR signals file\nVersion: 1\n#\n#\nname: Unknown\n"
        "type: raw\nfrequency: 38000\nduty_cycle: 0.330000\ndata:");
    if (written < 0 || (size_t)written >= out_size) {
        result = BRUCE_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    used = (size_t)written;
    for (size_t i = 0; i < event.num_symbols; ++i) {
        const rmt_symbol_word_t *symbol = &event.received_symbols[i];
        written = snprintf(out + used, out_size - used, " %u %u", symbol->duration0, symbol->duration1);
        if (written < 0 || (size_t)written >= out_size - used) {
            result = BRUCE_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
        used += (size_t)written;
    }
    written = snprintf(out + used, out_size - used, "\n#\n");
    result = written >= 0 && (size_t)written < out_size - used ? BRUCE_OK : BRUCE_ERR_RESOURCE_LIMIT;

cleanup:
    if (channel != NULL) {
        (void)rmt_disable(channel);
        (void)rmt_del_channel(channel);
    }
    if (queue != NULL) vQueueDelete(queue);
    free(symbols);
    ir__unlock();
    return result;
}

static char *ir__trim(char *text)
{
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static uint32_t ir__parse_hex_bytes_le(const char *text)
{
    uint32_t value = 0;
    unsigned int byte = 0;
    for (unsigned int index = 0; index < 4 && text != NULL && *text != '\0'; ++index) {
        while (*text == ' ' || *text == '\t') text++;
        if (sscanf(text, "%2x", &byte) != 1) break;
        value |= (uint32_t)(byte & 0xffu) << (index * 8u);
        for (int digit = 0; digit < 2 && isxdigit((unsigned char)*text); ++digit) text++;
    }
    return value;
}

static bool ir__normalize_hex(const char *text, char *out, size_t out_size)
{
    if (text == NULL || out_size < 2) return false;
    size_t used = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    while (*text != '\0') {
        if (isxdigit((unsigned char)*text)) {
            if (used + 1 >= out_size) return false;
            out[used++] = *text;
        } else if (!isspace((unsigned char)*text)) {
            return false;
        }
        text++;
    }
    out[used] = '\0';
    return used > 0;
}

bruce_result_t ir__transmit_parsed(const char *protocol, const char *address_hex,
                                   const char *command_hex, uint8_t repeats)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_IR);
    if (permission != BRUCE_OK) return permission;
    if (protocol == NULL || address_hex == NULL || command_hex == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    char normalized_address[9];
    char normalized_command[9];
    if (!ir__normalize_hex(address_hex, normalized_address, sizeof(normalized_address)) ||
        !ir__normalize_hex(command_hex, normalized_command, sizeof(normalized_command))) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    uint32_t address = ir__parse_hex_bytes_le(normalized_address);
    uint32_t command = ir__parse_hex_bytes_le(normalized_command);
    uint8_t address_byte = (uint8_t)address;
    uint8_t command_byte = (uint8_t)command;
    uint32_t frame;
    uint8_t bits = 32;
    if (strncasecmp(protocol, "SIRC", 4) == 0 || strcasecmp(protocol, "SONY") == 0) {
        frame = ((address & 0x1fffu) << 7) | (command & 0x7fu);
        bits = strcasecmp(protocol, "SIRC15") == 0 ? 15 :
               strcasecmp(protocol, "SIRC20") == 0 ? 20 : 12;
    } else if (strcasecmp(protocol, "NECext") == 0) {
        frame = ((address & 0xffffu) << 16) | ((uint32_t)command_byte << 8) | (uint8_t)~command_byte;
    } else if (strcasecmp(protocol, "Samsung32") == 0) {
        frame = ((uint32_t)address_byte << 24) | ((uint32_t)address_byte << 16) |
                ((uint32_t)command_byte << 8) | (uint8_t)~command_byte;
    } else if (strcasecmp(protocol, "NEC") == 0) {
        frame = ((uint32_t)address_byte << 24) | ((uint32_t)(uint8_t)~address_byte << 16) |
                ((uint32_t)command_byte << 8) | (uint8_t)~command_byte;
    } else {
        return BRUCE_ERR_UNSUPPORTED;
    }
    char payload[9];
    snprintf(payload, sizeof(payload), "%08" PRIX32, frame);
    return ir__transmit(payload, protocol, bits, repeats);
}

static bruce_result_t ir__send_file_record(char *record, uint8_t repeats)
{
    char *type = NULL;
    char *protocol = NULL;
    char *value = NULL;
    char *data = NULL;
    char *address = NULL;
    char *command = NULL;
    uint32_t frequency = BRUCE_IR_DEFAULT_FREQUENCY_HZ;
    uint8_t bits = 32;

    char *save = NULL;
    for (char *line = strtok_r(record, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (colon == NULL) continue;
        *colon = '\0';
        char *key = ir__trim(line);
        char *field = ir__trim(colon + 1);
        if (strcasecmp(key, "type") == 0) type = field;
        else if (strcasecmp(key, "protocol") == 0) protocol = field;
        else if (strcasecmp(key, "value") == 0 || strcasecmp(key, "state") == 0) value = field;
        else if (strcasecmp(key, "data") == 0) data = field;
        else if (strcasecmp(key, "address") == 0) address = field;
        else if (strcasecmp(key, "command") == 0) command = field;
        else if (strcasecmp(key, "frequency") == 0) frequency = (uint32_t)strtoul(field, NULL, 10);
        else if (strcasecmp(key, "bits") == 0) bits = (uint8_t)strtoul(field, NULL, 10);
    }
    if (type == NULL) return BRUCE_ERR_NOT_FOUND;
    if (strcasecmp(type, "parsed") == 0) {
        const char *payload = value != NULL ? value : data;
        char normalized[17];
        if (payload == NULL && address != NULL && command != NULL && protocol != NULL) {
            return ir__transmit_parsed(protocol, address, command, repeats);
        }
        if (payload != NULL && !ir__normalize_hex(payload, normalized, sizeof(normalized))) {
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        if (payload != NULL) payload = normalized;
        return ir__transmit(payload, protocol, bits, repeats);
    }
    if (strcasecmp(type, "raw") != 0 || data == NULL) return BRUCE_ERR_UNSUPPORTED;

    uint32_t timings[BRUCE_IR_MAX_RAW_TIMINGS];
    size_t count = 0;
    char *timing_save = NULL;
    for (char *token = strtok_r(data, " ,\t\r", &timing_save); token != NULL;
         token = strtok_r(NULL, " ,\t\r", &timing_save)) {
        if (count >= BRUCE_IR_MAX_RAW_TIMINGS) return BRUCE_ERR_RESOURCE_LIMIT;
        char *end = NULL;
        unsigned long duration = strtoul(token, &end, 10);
        if (end == token || *end != '\0' || duration == 0 || duration > UINT32_MAX) return BRUCE_ERR_INVALID_ARGUMENT;
        timings[count++] = (uint32_t)duration;
    }
    return ir__transmit_raw(timings, count, frequency, repeats);
}

bruce_result_t ir__transmit_file(const char *path, uint8_t repeats)
{
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_IR);
    if (permission != BRUCE_OK) return permission;
    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL) return BRUCE_ERR_INVALID_PATH;

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;
    char *contents = malloc(IR_FILE_MAX_SIZE + 1u);
    if (contents == NULL) {
        storage__close(file);
        return BRUCE_ERR_NO_MEMORY;
    }
    size_t total = 0;
    while (total < IR_FILE_MAX_SIZE) {
        size_t read_size = 0;
        result = storage__read(file, contents + total, IR_FILE_MAX_SIZE - total, &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        total += read_size;
    }
    if (result == BRUCE_OK && total == IR_FILE_MAX_SIZE) {
        char extra;
        size_t extra_size = 0;
        result = storage__read(file, &extra, 1, &extra_size);
        if (result == BRUCE_OK && extra_size != 0) result = BRUCE_ERR_RESOURCE_LIMIT;
    }
    (void)storage__close(file);
    if (result != BRUCE_OK) {
        free(contents);
        return result;
    }
    contents[total] = '\0';

    bool sent = false;
    char *cursor = contents;
    while (*cursor != '\0') {
        char *delimiter = strstr(cursor, "\n#");
        if (delimiter == NULL) delimiter = cursor + strlen(cursor);
        char saved = *delimiter;
        *delimiter = '\0';
        bruce_result_t record_result = ir__send_file_record(cursor, repeats);
        if (record_result == BRUCE_OK) sent = true;
        else if (record_result != BRUCE_ERR_NOT_FOUND) {
            free(contents);
            return record_result;
        }
        if (saved == '\0') break;
        cursor = delimiter + 2;
        while (*cursor == '\r' || *cursor == '\n' || *cursor == '#') cursor++;
    }
    free(contents);
    return sent ? BRUCE_OK : BRUCE_ERR_NOT_FOUND;
}
