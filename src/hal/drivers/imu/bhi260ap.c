#include "bhi260ap.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_i2c.h"

/* Registers, bits, commands and the upload framing from Bosch's BHY2
 * sensor API (bhy2_defs.h, bhy2_hif.c, BSD-3-Clause). */
#define BHI260AP_CMD_REG 0x00
#define BHI260AP_RESET_REG 0x14
#define BHI260AP_PRODUCT_ID_REG 0x1C
#define BHI260AP_REVISION_ID_REG 0x1D
#define BHI260AP_ROM_VERSION_REG 0x1E    /* 2 bytes */
#define BHI260AP_KERNEL_VERSION_REG 0x20 /* 2 bytes, 0 while no firmware runs */
#define BHI260AP_FEATURE_STATUS_REG 0x24
#define BHI260AP_BOOT_STATUS_REG 0x25
#define BHI260AP_PRODUCT_ID 0x89
#define BHI260AP_FLASH_DETECTED 0x01
#define BHI260AP_FLASH_VERIFY_DONE 0x02
#define BHI260AP_FLASH_VERIFY_ERROR 0x04
#define BHI260AP_NO_FLASH 0x08
#define BHI260AP_HOST_INTERFACE_READY 0x10
#define BHI260AP_FW_VERIFY_DONE 0x20
#define BHI260AP_FW_VERIFY_ERROR 0x40
#define BHI260AP_CMD_UPLOAD_TO_RAM 0x0002
#define BHI260AP_CMD_BOOT_RAM 0x0003
#define BHI260AP_FW_MAGIC 0x662B
#define BHI260AP_FRAME 252       /* Bosch's 256-byte command packet less its header. */
#define BHI260AP_READY_TRIES 15  /* 10 ms each, as Bosch's reset waits. */
#define BHI260AP_VERIFY_TRIES 100 /* 50 ms each, as Bosch's boot check waits. */
#define BHI260AP_SPEED_HZ 400000
#define BHI260AP_TIMEOUT_MS 100

static const char *TAG = "bhi260ap";
static const char *const bhi260ap__firmware_paths[] = {"/lib/firmware/bhi260ap.fw",
                                                        "/sdcard/lib/firmware/bhi260ap.fw"};

typedef struct {
    i2c_master_dev_handle_t i2c;
    uint8_t revision;
    uint16_t rom;
    uint16_t kernel;
    uint8_t feature;
    uint8_t boot;
} bhi260ap_t;

static bool bhi260ap__read(bhi260ap_t *chip, uint8_t reg, uint8_t *out, size_t length) {
    return i2c_master_transmit_receive(chip->i2c, &reg, 1, out, length, BHI260AP_TIMEOUT_MS) == ESP_OK;
}

static bool bhi260ap__read_u16(bhi260ap_t *chip, uint8_t reg, uint16_t *out) {
    uint8_t value[2];
    if (!bhi260ap__read(chip, reg, value, sizeof(value))) return false;
    *out = (uint16_t)(value[0] | value[1] << 8);
    return true;
}

/* Until the host interface answers: after power-up or a reset. */
static bool bhi260ap__wait_ready(bhi260ap_t *chip) {
    for (int i = 0; i < BHI260AP_READY_TRIES; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (bhi260ap__read(chip, BHI260AP_BOOT_STATUS_REG, &chip->boot, 1) &&
            (chip->boot & BHI260AP_HOST_INTERFACE_READY)) {
            return true;
        }
    }
    return false;
}

/* After an upload or a boot command: until the firmware checks out. */
static bool bhi260ap__wait_verified(bhi260ap_t *chip) {
    for (int i = 0; i < BHI260AP_VERIFY_TRIES; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!bhi260ap__read(chip, BHI260AP_BOOT_STATUS_REG, &chip->boot, 1)) continue;
        if (chip->boot & BHI260AP_FW_VERIFY_ERROR) return false;
        if ((chip->boot & BHI260AP_HOST_INTERFACE_READY) && (chip->boot & BHI260AP_FW_VERIFY_DONE)) return true;
    }
    return false;
}

static FILE *bhi260ap__open_firmware(long *size) {
    for (size_t i = 0; i < sizeof(bhi260ap__firmware_paths) / sizeof(bhi260ap__firmware_paths[0]); ++i) {
        FILE *file = fopen(bhi260ap__firmware_paths[i], "rb");
        if (file == NULL) continue;
        if (fseek(file, 0, SEEK_END) == 0 && (*size = ftell(file)) > 4 && fseek(file, 0, SEEK_SET) == 0) {
            ESP_LOGI(TAG, "uploading %s (%ld bytes)", bhi260ap__firmware_paths[i], *size);
            return file;
        }
        fclose(file);
    }
    return NULL;
}

/* One command on the command channel: a 4-byte header (command, then the
 * length: in 32-bit words for an upload), then the payload, in frames of
 * up to BHI260AP_FRAME bytes, the last padded to whole words. */
static bruce_result_t bhi260ap__upload(bhi260ap_t *chip) {
    long size = 0;
    FILE *file = bhi260ap__open_firmware(&size);
    if (file == NULL) return BRUCE_ERR_NOT_FOUND;
    uint32_t words = ((uint32_t)size + 3) / 4;
    uint8_t *frame = malloc(1 + BHI260AP_FRAME);
    bruce_result_t result = words <= 0xFFFF && frame != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
    if (result == BRUCE_OK) {
        frame[0] = BHI260AP_CMD_REG;
        frame[1] = (uint8_t)BHI260AP_CMD_UPLOAD_TO_RAM;
        frame[2] = (uint8_t)(BHI260AP_CMD_UPLOAD_TO_RAM >> 8);
        frame[3] = (uint8_t)words;
        frame[4] = (uint8_t)(words >> 8);
    }
    size_t header = 4;
    long left = size;
    while (result == BRUCE_OK && left > 0) {
        size_t room = BHI260AP_FRAME - header;
        size_t take = left < (long)room ? (size_t)left : room;
        if (fread(&frame[1 + header], 1, take, file) != take) {
            result = BRUCE_ERR_IO;
            break;
        }
        if (left == size && (frame[5] | frame[6] << 8) != BHI260AP_FW_MAGIC) {
            result = BRUCE_ERR_INVALID_ARGUMENT;
            break;
        }
        size_t length = header + take;
        while (length % 4) frame[1 + length++] = 0;
        if (i2c_master_transmit(chip->i2c, frame, 1 + length, BHI260AP_TIMEOUT_MS) != ESP_OK) result = BRUCE_ERR_IO;
        left -= (long)take;
        header = 0;
    }
    free(frame);
    fclose(file);
    if (result == BRUCE_OK && !bhi260ap__wait_verified(chip)) result = BRUCE_ERR_IO;
    return result;
}

static bruce_result_t bhi260ap__boot(bhi260ap_t *chip) {
    const uint8_t command[] = {BHI260AP_CMD_REG, (uint8_t)BHI260AP_CMD_BOOT_RAM, 0, 0, 0};
    if (i2c_master_transmit(chip->i2c, command, sizeof(command), BHI260AP_TIMEOUT_MS) != ESP_OK) return BRUCE_ERR_IO;
    if (!bhi260ap__wait_verified(chip)) return BRUCE_ERR_IO;
    return bhi260ap__read_u16(chip, BHI260AP_KERNEL_VERSION_REG, &chip->kernel) && chip->kernel != 0 ? BRUCE_OK
                                                                                                    : BRUCE_ERR_IO;
}

static bruce_result_t bhi260ap__probe(const hal_device_t *device) {
    bhi260ap_t *chip = calloc(1, sizeof(*chip));
    if (chip == NULL) return BRUCE_ERR_NO_MEMORY;
    bruce_result_t result = hal_i2c_open(device, BHI260AP_SPEED_HZ, &chip->i2c);
    if (result == BRUCE_OK) result = hal_device_power(device, true);
    uint8_t product = 0;
    if (result == BRUCE_OK && (!bhi260ap__wait_ready(chip) ||
                               !bhi260ap__read(chip, BHI260AP_PRODUCT_ID_REG, &product, 1) ||
                               product != BHI260AP_PRODUCT_ID)) {
        result = BRUCE_ERR_NOT_FOUND;
    }
    if (result == BRUCE_OK) {
        (void)bhi260ap__read(chip, BHI260AP_REVISION_ID_REG, &chip->revision, 1);
        (void)bhi260ap__read_u16(chip, BHI260AP_ROM_VERSION_REG, &chip->rom);
        (void)bhi260ap__read(chip, BHI260AP_FEATURE_STATUS_REG, &chip->feature, 1);
    }
    /* Off until a user starts it. */
    (void)hal_device_power(device, false);
    if (result != BRUCE_OK) {
        if (chip->i2c != NULL) i2c_master_bus_rm_device(chip->i2c);
        free(chip);
        return result;
    }
    device->state->data = chip;
    return BRUCE_OK;
}

/* Powers it and, unless its firmware still runs, uploads and boots it. */
static bruce_result_t bhi260ap__start(const hal_device_t *device) {
    bhi260ap_t *chip = device->state->data;
    bruce_result_t result = hal_device_power(device, true);
    if (result != BRUCE_OK) return result;
    if (!bhi260ap__wait_ready(chip)) return BRUCE_ERR_IO;
    if (bhi260ap__read_u16(chip, BHI260AP_KERNEL_VERSION_REG, &chip->kernel) && chip->kernel != 0) return BRUCE_OK;
    /* From a clean boot ROM, as Bosch's driver starts. */
    const uint8_t reset[] = {BHI260AP_RESET_REG, 0x01};
    if (i2c_master_transmit(chip->i2c, reset, sizeof(reset), BHI260AP_TIMEOUT_MS) != ESP_OK ||
        !bhi260ap__wait_ready(chip)) {
        return BRUCE_ERR_IO;
    }
    result = bhi260ap__upload(chip);
    if (result == BRUCE_OK) result = bhi260ap__boot(chip);
    if (result == BRUCE_OK) ESP_LOGI(TAG, "firmware running, kernel %u", chip->kernel);
    else ESP_LOGW(TAG, "firmware upload failed (%d), boot status 0x%02x", (int)result, chip->boot);
    return result;
}

static void bhi260ap__describe(const hal_device_t *device, char *out, size_t size) {
    const bhi260ap_t *chip = device->state->data;
    const char *flash = chip->boot & BHI260AP_NO_FLASH             ? "no flash"
                        : chip->boot & BHI260AP_FLASH_VERIFY_ERROR ? "flash, verify failed"
                        : chip->boot & BHI260AP_FLASH_VERIFY_DONE  ? "flash, verified"
                        : chip->boot & BHI260AP_FLASH_DETECTED     ? "flash"
                                                                    : "flash unknown";
    snprintf(out, size, "rev 0x%02x, rom %u, kernel %u, boot 0x%02x (%s), feature 0x%02x", chip->revision,
             chip->rom, chip->kernel, chip->boot, flash, chip->feature);
}

const hal_driver_t bhi260ap_driver = {
    .name = "bhi260ap",
    .class_id = HAL_CLASS_NONE,
    .probe = bhi260ap__probe,
    .start = bhi260ap__start,
    .describe = bhi260ap__describe,
};
