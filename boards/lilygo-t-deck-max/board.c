/* LilyGO T-Deck MAX: what the HAL drives so far. Wiring from LilyGO's
 * TDeckMaxBoard.h; more devices move here as their drivers join the HAL. */

#include "hal/drivers/gpio/xl9555.h"
#include "hal/hal.h"
#include "hal/hal_i2c.h"
#include "hal/hal_rail.h"
#include "hal/hal_spi.h"

/* Keyboard, touch, XL9555, ES8311, BQ27220. */
static const hal_i2c_master_config_t i2c0_config = {.port = 0, .sda = 13, .scl = 14, .pullup = true};
HAL_DEVICE(i2c0, .driver = &hal_i2c_master_driver, .config = &i2c0_config);

/* SD card (CS 48), e-paper (CS 34) and LoRa (CS 3); their drivers add
 * themselves to it. Transfers up to 4096 bytes, as they set it up before. */
static const hal_spi_master_config_t spi2_config = {.host = SPI2_HOST, .sck = 36, .mosi = 33, .miso = 47, .max_transfer = 4096};
HAL_DEVICE(spi2, .driver = &hal_spi_master_driver, .config = &spi2_config);

/* Outputs at boot: 4 internal LoRa antenna, 6 speaker amplifier on,
 * 7 touch out of reset, 9 keyboard out of reset. */
static const xl9555_config_t xl9555_config = {.levels = (1u << 4) | (1u << 6) | (1u << 7) | (1u << 9)};
HAL_DEVICE(xl9555, .driver = &xl9555_driver, .bus = &i2c0, .address = 0x20, .config = &xl9555_config);

HAL_RAIL(modem_power, .enable = HAL_PIN(xl9555, 0));
HAL_RAIL(gps_power, .enable = HAL_PIN(xl9555, 2));
HAL_RAIL(imu_power, .enable = HAL_PIN(xl9555, 3)); /* BHI260AP's 1.8 V, for when it has a driver. */

/* No drivers yet: core/gps and core/cellular drive them through these. */
HAL_DEVICE(gps, HAL_RAILS(&gps_power));
HAL_DEVICE(modem,
           HAL_PINS({"pwrkey", HAL_PIN(xl9555, 8)}, /* High presses the A7682E's power key. */
                    {"audio-select", HAL_PIN(xl9555, 10)}), /* High: speaker and mic to the modem. */
           HAL_RAILS(&modem_power));

HAL_BOARD("LilyGO T-Deck MAX", &i2c0, &spi2, &xl9555, &modem_power, &gps_power, &imu_power, &gps, &modem);
