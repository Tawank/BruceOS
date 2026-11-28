#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Sub-GHz radio modules (SX1262, CC1101...) the board has.
 *
 * Any app can list the modules; one at a time holds a module between
 * rf__open() and rf__close() (or its exit) to use it.
 */

#define BRUCE_RF_NAME_MAX 16
#define BRUCE_RF_BANDS_MAX 3

/** A range the module tunes, in kHz. */
typedef struct {
    uint32_t min_khz;
    uint32_t max_khz;
} bruce_rf_band_t;

typedef struct {
    char name[BRUCE_RF_NAME_MAX]; /* The board's name for it, e.g. "lora". */
    char chip[BRUCE_RF_NAME_MAX]; /* Its driver, e.g. "sx1262". */
    bruce_rf_band_t bands[BRUCE_RF_BANDS_MAX];
    uint8_t band_count;
    bool in_use; /* Another process holds it. */
} bruce_rf_module_t;

/** @brief How many RF modules the board has. */
size_t rf__count(void);

/**
 * @brief Describes module `index` (0 to rf__count() - 1).
 */
bruce_result_t rf__info(size_t index, bruce_rf_module_t *out_module);

/**
 * @brief Powers module `index` up for the calling process.
 *
 * BRUCE_ERR_BUSY while another process holds it.
 * @permission rf
 */
bruce_result_t rf__open(size_t index);

/** @brief Lets module `index` go and powers it down. */
bruce_result_t rf__close(size_t index);

/**
 * @brief Receives on `frequency_khz` and measures the signal there, in dBm.
 *
 * Only for the process that opened the module. BRUCE_ERR_INVALID_ARGUMENT
 * when the frequency is outside its bands.
 * @permission rf
 */
bruce_result_t rf__rssi(size_t index, uint32_t frequency_khz, int16_t *out_dbm);
