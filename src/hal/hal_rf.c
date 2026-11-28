#include "hal_rf.h"

static const hal_rf_api_t *hal_rf__api(const hal_device_t *device) {
    if (!hal_device_ready(device) || device->driver == NULL || device->driver->class_id != HAL_CLASS_RF) return NULL;
    return device->driver->api;
}

size_t hal_rf_bands(const hal_device_t *device, const hal_rf_band_t **out_bands) {
    const hal_rf_api_t *api = hal_rf__api(device);
    if (api == NULL) return 0;
    return api->bands(device, out_bands);
}

bool hal_rf_tunes(const hal_device_t *device, uint32_t frequency_khz) {
    const hal_rf_band_t *bands = NULL;
    size_t count = hal_rf_bands(device, &bands);
    for (size_t i = 0; i < count; ++i) {
        if (frequency_khz >= bands[i].min_khz && frequency_khz <= bands[i].max_khz) return true;
    }
    return false;
}

bruce_result_t hal_rf_rssi(const hal_device_t *device, uint32_t frequency_khz, int16_t *out_dbm) {
    const hal_rf_api_t *api = hal_rf__api(device);
    if (api == NULL) return BRUCE_ERR_NOT_FOUND;
    if (out_dbm == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!hal_rf_tunes(device, frequency_khz)) return BRUCE_ERR_INVALID_ARGUMENT;
    return api->rssi(device, frequency_khz, out_dbm);
}
