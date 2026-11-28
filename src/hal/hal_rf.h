#pragma once

#include "hal.h"

#define HAL_RF_BANDS_MAX 3

/* A range the radio tunes, in kHz. */
typedef struct {
    uint32_t min_khz;
    uint32_t max_khz;
} hal_rf_band_t;

/* Operations of a HAL_CLASS_RF driver (a sub-GHz transceiver); `rssi`
 * between hal_device_get() and put(). */
typedef struct {
    size_t (*bands)(const hal_device_t *device, const hal_rf_band_t **out_bands);
    /* Receives on `frequency_khz` and measures the signal there. */
    bruce_result_t (*rssi)(const hal_device_t *device, uint32_t frequency_khz, int16_t *out_dbm);
} hal_rf_api_t;

/* The bands `device` tunes; 0 when it isn't an RF device. */
size_t hal_rf_bands(const hal_device_t *device, const hal_rf_band_t **out_bands);
bool hal_rf_tunes(const hal_device_t *device, uint32_t frequency_khz);
bruce_result_t hal_rf_rssi(const hal_device_t *device, uint32_t frequency_khz, int16_t *out_dbm);
