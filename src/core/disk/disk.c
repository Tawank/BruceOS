#include "disk.h"

#include <string.h>

#include "core/process/process.h"
#include "core/storage/storage.h"
#include "driver/spi_master.h" // IWYU pragma: export
#include "esp_flash.h"
#include "esp_partition.h"

static void disk__set_text(char *destination, size_t capacity, const char *value) {
    if (capacity == 0) return;
    strncpy(destination, value, capacity - 1);
    destination[capacity - 1] = '\0';
}

static void disk__write_entry(
    bruce_disk_entry_t *entries, size_t capacity, size_t index, const char *name, const char *parent,
    const char *mount_point, bruce_disk_type_t type, uint64_t offset, uint64_t size, bool removable
) {
    if (entries == NULL || index >= capacity) return;
    bruce_disk_entry_t *entry = &entries[index];
    memset(entry, 0, sizeof(*entry));
    disk__set_text(entry->name, sizeof(entry->name), name);
    disk__set_text(entry->parent, sizeof(entry->parent), parent);
    disk__set_text(entry->mount_point, sizeof(entry->mount_point), mount_point);
    entry->type = type;
    entry->offset = offset;
    entry->size = size;
    entry->removable = removable;
}

static bool disk__caller_is_built_in(void) {
    bool built_in = false;
    return process_registry__current_context(&built_in, NULL, 0, NULL) == BRUCE_OK && built_in;
}

bruce_result_t disk__list(bruce_disk_entry_t *entries, size_t capacity, size_t *out_count) {
    if (out_count == NULL || (entries == NULL && capacity != 0)) return BRUCE_ERR_INVALID_ARGUMENT;

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) return BRUCE_ERR_IO;

    size_t count = 0;
    disk__write_entry(
        entries, capacity, count++, "flash0", "", "", BRUCE_DISK_TYPE_DISK, 0, flash_size, false
    );

    esp_partition_iterator_t iterator =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        const char *mount_point = storage__is_internal_partition_mounted(partition->label) ? "/" : "";
        disk__write_entry(
            entries,
            capacity,
            count++,
            partition->label,
            "flash0",
            mount_point,
            BRUCE_DISK_TYPE_PARTITION,
            partition->address,
            partition->size,
            false
        );
        iterator = esp_partition_next(iterator);
    }

    uint64_t sd_size = 0;
    if (storage__get_sd_capacity(&sd_size)) {
        disk__write_entry(
            entries, capacity, count++, "sd0", "", "/sdcard", BRUCE_DISK_TYPE_DISK, 0, sd_size, true
        );
    }

    *out_count = count;
    return capacity < count && entries != NULL ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_OK;
}

bruce_result_t disk__mount(const char *name, const char *mount_point) {
    if (name == NULL || mount_point == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!disk__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (strcmp(name, "sd0") != 0) return BRUCE_ERR_NOT_FOUND;
    if (strcmp(mount_point, "/sdcard") != 0) return BRUCE_ERR_INVALID_PATH;

#if CONFIG_BRUCE_SD_ENABLED
    const storage__sdspi_config_t config = {
        .host = (spi_host_device_t)CONFIG_BRUCE_SD_SPI_HOST,
        .mosi_gpio = CONFIG_BRUCE_SD_PIN_MOSI,
        .miso_gpio = CONFIG_BRUCE_SD_PIN_MISO,
        .sck_gpio = CONFIG_BRUCE_SD_PIN_SCK,
        .cs_gpio = CONFIG_BRUCE_SD_PIN_CS,
    };
    return storage__sd_mount_spi(&config) ? BRUCE_OK : BRUCE_ERR_IO;
#else
    return BRUCE_ERR_UNSUPPORTED;
#endif
}

bruce_result_t disk__unmount(const char *name_or_mount_point) {
    if (name_or_mount_point == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!disk__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (strcmp(name_or_mount_point, "sd0") != 0 && strcmp(name_or_mount_point, "/sdcard") != 0) {
        return BRUCE_ERR_NOT_FOUND;
    }
    return storage__sd_unmount();
}
