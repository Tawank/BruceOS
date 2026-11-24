/* LilyGO T-Deck MAX: what the HAL drives so far. Wiring from LilyGO's
 * TDeckMaxBoard.h; more devices move here as their drivers join the HAL. */

#include "hal/drivers/battery/bq27220.h"
#include "hal/drivers/gpio/xl9555.h"
#include "hal/drivers/haptic/drv2605.h"
#include "hal/drivers/imu/bhi260ap.h"
#include "hal/drivers/keypad/tca8418.h"
#include "hal/drivers/touch/cst3530.h"
#include "hal/hal.h"
#include "hal/hal_hog.h"
#include "hal/hal_i2c.h"
#include "hal/hal_rail.h"
#include "hal/hal_spi.h"

/* Off the shared SPI bus from boot, as LilyGO's firmware does: LoRa (no
 * driver yet), and the SD card and e-paper until their drivers take over. */
static const hal_hog_config_t spi2_quiet_config = {.active = false};
HAL_DEVICE(spi2_quiet, .driver = &hal_hog_driver, .config = &spi2_quiet_config,
           HAL_PINS({"lora-cs", HAL_SOC_PIN_LOW(3)}, {"lora-reset", HAL_SOC_PIN_LOW(4)},
                    {"sd-cs", HAL_SOC_PIN_LOW(48)}, {"display-cs", HAL_SOC_PIN_LOW(34)}));

/* Keyboard, touch, XL9555, ES8311, BQ27220. */
static const hal_i2c_master_config_t i2c0_config = {.port = 0, .sda = 13, .scl = 14, .pullup = true, .speed_hz = 400000};
HAL_DEVICE(i2c0, .driver = &hal_i2c_master_driver, .config = &i2c0_config);

/* SD card (CS 48), e-paper (CS 34) and LoRa (CS 3); their drivers add
 * themselves to it. Transfers up to 4096 bytes, as they set it up before. */
static const hal_spi_master_config_t spi2_config = {.host = SPI2_HOST, .sck = 36, .mosi = 33, .miso = 47, .max_transfer = 4096};
HAL_DEVICE(spi2, .driver = &hal_spi_master_driver, .config = &spi2_config);

/* No drivers yet: core/storage and core/display set them up on spi2. */
HAL_DEVICE(sd, .bus = &spi2, HAL_PINS({"cs", HAL_SOC_PIN_LOW(48)}));
/* GDEQ031T10 (UC8253). BUSY is low while the panel works; 41 is the front light. */
HAL_DEVICE(display, .bus = &spi2,
           HAL_PINS({"cs", HAL_SOC_PIN_LOW(34)}, {"dc", HAL_SOC_PIN(35)}, {"reset", HAL_SOC_PIN_LOW(9)},
                    {"busy", HAL_SOC_PIN_LOW(37)}, {"backlight", HAL_SOC_PIN(41)}));

/* Outputs at boot: 4 internal LoRa antenna, 6 speaker amplifier on,
 * 7 touch and 9 keyboard out of reset, whichever code then drives them. */
static const xl9555_config_t xl9555_config = {.levels = (1u << 4) | (1u << 6) | (1u << 7) | (1u << 9)};
HAL_DEVICE(xl9555, .driver = &xl9555_driver, .bus = &i2c0, .address = 0x20, .config = &xl9555_config);

HAL_DEVICE(battery, .driver = &bq27220_driver, .bus = &i2c0, .address = 0x55);

HAL_DEVICE(touch, .driver = &cst3530_driver, .bus = &i2c0, .address = 0x1A,
           HAL_PINS({"reset", HAL_PIN_LOW(xl9555, 7)}));

/* 4 rows and 10 columns, BlackBerry style. */
static const tca8418_config_t keyboard_config = {.rows = 0x0F, .cols = 0x03FF};
HAL_DEVICE(keyboard, .driver = &tca8418_driver, .bus = &i2c0, .address = 0x34, .config = &keyboard_config,
           HAL_PINS({"reset", HAL_PIN_LOW(xl9555, 9)}));

/* Kept through an ESP restart: the modem stays registered, the IMU keeps its firmware. */
HAL_RAIL(modem_power, .enable = HAL_PIN(xl9555, 0), .keep = true);
HAL_RAIL(gps_power, .enable = HAL_PIN(xl9555, 2));
HAL_RAIL(imu_power, .enable = HAL_PIN(xl9555, 3), .keep = true); /* BHI260AP's 1.8 V. */

/* No reset line; HIRQ on GPIO 21 for when it runs firmware. */
HAL_DEVICE(imu, .driver = &bhi260ap_driver, .bus = &i2c0, .address = 0x28,
           HAL_PINS({"interrupt", HAL_SOC_PIN(21)}), HAL_RAILS(&imu_power));

/* Vibration motor: the DRV2605's power and enable (M_EN). */
HAL_RAIL(motor_power, .enable = HAL_PIN(xl9555, 5));
HAL_DEVICE(motor, .driver = &drv2605_driver, .bus = &i2c0, .address = 0x5A, HAL_RAILS(&motor_power));

/* No drivers yet: core/gps and core/cellular drive them through these. */
HAL_DEVICE(gps, HAL_RAILS(&gps_power));
HAL_DEVICE(modem,
           HAL_PINS({"pwrkey", HAL_PIN(xl9555, 8)}, /* High presses the A7682E's power key. */
                    {"audio-select", HAL_PIN(xl9555, 10)}), /* High: speaker and mic to the modem. */
           HAL_RAILS(&modem_power));

HAL_BOARD("LilyGO T-Deck MAX", &spi2_quiet, &i2c0, &spi2, &sd, &display, &xl9555, &battery, &touch, &keyboard, &modem_power, &gps_power, &imu_power, &imu, &motor_power, &motor, &gps, &modem);
