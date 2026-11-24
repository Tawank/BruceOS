#include "bhi260ap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "hal/hal_i2c.h"
#include "hal/hal_imu.h"

/* Registers, bits, commands and the upload framing from Bosch's BHY2
 * sensor API (bhy2_defs.h, bhy2_hif.c, BSD-3-Clause). */
#define BHI260AP_CMD_REG 0x00
#define BHI260AP_FIFO_W_REG 0x01  /* Wake-up FIFO, the same way: boot and meta events. */
#define BHI260AP_FIFO_NW_REG 0x02 /* Non-wake-up FIFO: 2-byte count, then the data. */
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
#define BHI260AP_CMD_CONFIG_SENSOR 0x000D
/* Game rotation vector: orientation from accel and gyro, no magnetometer.
 * A quaternion x, y, z, w (int16, 1/16384) and an accuracy (uint16). */
#define BHI260AP_GAME_RV 37
#define BHI260AP_GAME_RV_HZ 25.0f
#define BHI260AP_QUATERNION_SCALE 16384.0f
#define BHI260AP_CONFIG_RETRY_MS 1000 /* No sample yet: send the sensor's configuration again. */
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
    hal_imu_reading_t reading;
    uint16_t fifo_kept; /* Bytes of a record split between reads. */
    uint8_t fifo[128];
    TickType_t configured_at;
    uint32_t samples;
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

/* A command with a byte-length payload of whole words. */
static bool bhi260ap__command(bhi260ap_t *chip, uint16_t command, const uint8_t *payload, uint8_t length) {
    uint8_t frame[1 + 4 + 8] = {BHI260AP_CMD_REG, (uint8_t)command, (uint8_t)(command >> 8), length, 0};
    if (length > 8 || length % 4) return false;
    memcpy(&frame[5], payload, length);
    return i2c_master_transmit(chip->i2c, frame, 5 + length, BHI260AP_TIMEOUT_MS) == ESP_OK;
}

/* Sensor id, rate (float), latency in ms (24 bits); rate 0 turns it off. */
static bool bhi260ap__configure(bhi260ap_t *chip, uint8_t sensor, float rate_hz) {
    uint8_t payload[8] = {sensor};
    memcpy(&payload[1], &rate_hz, sizeof(rate_hz));
    return bhi260ap__command(chip, BHI260AP_CMD_CONFIG_SENSOR, payload, sizeof(payload));
}

/* Bytes of a FIFO record, id included; 0 for one this driver doesn't know.
 * System records from bhy2.c; the game rotation vector from bhy2_parse.c. */
static uint8_t bhi260ap__record_size(uint8_t id) {
    switch (id) {
        case 0:   /* Padding */
        case 255: /* Filler */ return 1;
        case 245: /* Timestamp, small delta (wake-up) */
        case 251: /* Timestamp, small delta */ return 2;
        case 246:
        case 252: /* Large delta */ return 3;
        case 247:
        case 253: /* Full timestamp */ return 6;
        case 248:
        case 254: /* Meta event */ return 4;
        case 250: /* Debug message */ return 18;
        case BHI260AP_GAME_RV: return 11;
        default: return 0;
    }
}

static void bhi260ap__quaternion(bhi260ap_t *chip, const uint8_t *data) {
    float z = (int16_t)(data[4] | data[5] << 8) / BHI260AP_QUATERNION_SCALE;
    float w = (int16_t)(data[6] | data[7] << 8) / BHI260AP_QUATERNION_SCALE;
    /* The twist about the vertical (world z): 2 atan2(z, w), whatever the tilt. */
    float yaw = 2.0f * atan2f(z, w) * 180.0f / (float)M_PI;
    if (yaw > 180.0f) yaw -= 360.0f;
    if (yaw <= -180.0f) yaw += 360.0f;
    chip->reading = (hal_imu_reading_t){.valid = true, .yaw_deg = yaw};
}

/* Reads all a FIFO holds, keeping the latest orientation. */
static bool bhi260ap__drain(bhi260ap_t *chip, uint8_t reg) {
    uint8_t count[2];
    if (!bhi260ap__read(chip, reg, count, sizeof(count))) return false;
    uint32_t left = count[0] | count[1] << 8;
    bool lost = false;
    chip->fifo_kept = 0;
    while (left > 0) {
        uint32_t take = sizeof(chip->fifo) - chip->fifo_kept;
        if (take > left) take = left;
        if (!bhi260ap__read(chip, reg, &chip->fifo[chip->fifo_kept], take)) return false;
        left -= take;
        uint32_t end = chip->fifo_kept + take;
        uint32_t pos = 0;
        /* After a record it doesn't know it can't find the next: read the rest away. */
        while (!lost && pos < end) {
            uint8_t size = bhi260ap__record_size(chip->fifo[pos]);
            if (size == 0) {
                lost = true;
                ESP_LOGW(TAG, "unknown FIFO record %u", chip->fifo[pos]);
            } else if (pos + size > end) {
                break;
            } else {
                uint8_t id = chip->fifo[pos];
                if (id == BHI260AP_GAME_RV) {
                    bhi260ap__quaternion(chip, &chip->fifo[pos + 1]);
                    chip->samples++;
                }
                pos += size;
            }
        }
        chip->fifo_kept = lost ? 0 : (uint16_t)(end - pos);
        memmove(chip->fifo, &chip->fifo[pos], chip->fifo_kept);
    }
    return true;
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
        (void)bhi260ap__read_u16(chip, BHI260AP_KERNEL_VERSION_REG, &chip->kernel);
    }
    /* Still running firmware from before an ESP restart (its rail kept): stays
     * powered with the sensor off. Otherwise off until a user starts it. */
    if (result == BRUCE_OK && chip->kernel != 0) {
        (void)bhi260ap__configure(chip, BHI260AP_GAME_RV, 0.0f);
    } else {
        chip->kernel = 0;
        (void)hal_device_power(device, false);
    }
    if (result != BRUCE_OK) {
        if (chip->i2c != NULL) i2c_master_bus_rm_device(chip->i2c);
        free(chip);
        return result;
    }
    device->state->data = chip;
    return BRUCE_OK;
}

static bruce_result_t bhi260ap__load(bhi260ap_t *chip);

/* Powers it, uploads and boots the firmware unless it still runs, and turns on the orientation sensor. */
static bruce_result_t bhi260ap__start(const hal_device_t *device) {
    bhi260ap_t *chip = device->state->data;
    bruce_result_t result = hal_device_power(device, true);
    if (result != BRUCE_OK) return result;
    if (!bhi260ap__wait_ready(chip)) return BRUCE_ERR_IO;
    chip->reading = (hal_imu_reading_t){0};
    if (!bhi260ap__read_u16(chip, BHI260AP_KERNEL_VERSION_REG, &chip->kernel) || chip->kernel == 0) {
        result = bhi260ap__load(chip);
        if (result != BRUCE_OK) return result;
    }
    chip->configured_at = xTaskGetTickCount();
    return bhi260ap__configure(chip, BHI260AP_GAME_RV, BHI260AP_GAME_RV_HZ) ? BRUCE_OK : BRUCE_ERR_IO;
}

/* The firmware stays loaded and the chip powered; only the sensor goes off. */
static void bhi260ap__stop(const hal_device_t *device) {
    bhi260ap_t *chip = device->state->data;
    (void)bhi260ap__configure(chip, BHI260AP_GAME_RV, 0.0f);
}

static bruce_result_t bhi260ap__read_imu(const hal_device_t *device, hal_imu_reading_t *out) {
    bhi260ap_t *chip = device->state->data;
    /* The wake-up FIFO carries boot and meta events only; emptied so it never fills. */
    bool drained = bhi260ap__drain(chip, BHI260AP_FIFO_W_REG) && bhi260ap__drain(chip, BHI260AP_FIFO_NW_REG);
    /* Right after the firmware boots the first configuration can be lost. */
    TickType_t now = xTaskGetTickCount();
    if (!chip->reading.valid && now - chip->configured_at >= pdMS_TO_TICKS(BHI260AP_CONFIG_RETRY_MS)) {
        chip->configured_at = now;
        (void)bhi260ap__configure(chip, BHI260AP_GAME_RV, BHI260AP_GAME_RV_HZ);
    }
    *out = chip->reading;
    return drained ? BRUCE_OK : BRUCE_ERR_IO;
}

/* Into RAM from a clean boot ROM, as Bosch's driver starts. */
static bruce_result_t bhi260ap__load(bhi260ap_t *chip) {
    bruce_result_t result;
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
    int used = snprintf(out, size, "rev 0x%02x, rom %u, kernel %u, boot 0x%02x (%s)", chip->revision, chip->rom,
                        chip->kernel, chip->boot, flash);
    if (chip->kernel != 0 && used > 0 && (size_t)used < size) {
        used += snprintf(out + used, size - used, ", %lu samples", (unsigned long)chip->samples);
    }
    if (chip->reading.valid && used > 0 && (size_t)used < size) {
        snprintf(out + used, size - used, ", yaw %.0f", (double)chip->reading.yaw_deg);
    }
}

static const hal_imu_api_t bhi260ap__api = {.read = bhi260ap__read_imu};

const hal_driver_t bhi260ap_driver = {
    .name = "bhi260ap",
    .class_id = HAL_CLASS_IMU,
    .probe = bhi260ap__probe,
    .start = bhi260ap__start,
    .stop = bhi260ap__stop,
    .describe = bhi260ap__describe,
    .api = &bhi260ap__api,
};
