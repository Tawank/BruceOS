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
        char mount_point[BRUCE_DISK_MOUNT_POINT_MAX] = "";
        storage__internal_mount_point(partition->label, mount_point, sizeof(mount_point));
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

/* Rejects anything that isn't an absolute, single-level-clean VFS path, plus
 * the two prefixes that are never available to a partition mount: "/sdcard"
 * (owned by sd0) and "/" itself (the root littlefs fallback VFS - too short
 * to be a VFS prefix at all, see STORAGE__MOUNT_PATH). */
static bool disk__mount_point_valid(const char *mount_point) {
    size_t length = strlen(mount_point);
    if (mount_point[0] != '/' || length < 2 || length >= BRUCE_DISK_MOUNT_POINT_MAX) return false;
    if (strcmp(mount_point, "/sdcard") == 0) return false;
    const char *component = mount_point + 1;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t component_length = end != NULL ? (size_t)(end - component) : strlen(component);
        if (component_length == 0 || (component_length == 1 && component[0] == '.') ||
            (component_length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (end == NULL) break;
        component = end + 1;
    }
    return true;
}

#if CONFIG_BRUCE_SD_ENABLED
/* Board-configured SD pins (Kconfig, set per-board in sdkconfig.defaults),
 * shared by disk__mount()'s "sd0" case and disk__mount_sd_boot() below so
 * the two entry points can never disagree on which pins to use. */
static bool disk__sd_mount_default(void) {
    const storage__sdspi_config_t config = {
        .host = (spi_host_device_t)CONFIG_BRUCE_SD_SPI_HOST,
        .mosi_gpio = CONFIG_BRUCE_SD_PIN_MOSI,
        .miso_gpio = CONFIG_BRUCE_SD_PIN_MISO,
        .sck_gpio = CONFIG_BRUCE_SD_PIN_SCK,
        .cs_gpio = CONFIG_BRUCE_SD_PIN_CS,
    };
    return storage__sd_mount_spi(&config);
}
#endif

bruce_result_t disk__mount(const char *name, const char *mount_point) {
    if (name == NULL || mount_point == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!disk__caller_is_built_in()) return BRUCE_ERR_PERMISSION;

    if (strcmp(name, "sd0") == 0) {
        if (strcmp(mount_point, "/sdcard") != 0) return BRUCE_ERR_INVALID_PATH;
#if CONFIG_BRUCE_SD_ENABLED
        return disk__sd_mount_default() ? BRUCE_OK : BRUCE_ERR_IO;
#else
        return BRUCE_ERR_UNSUPPORTED;
#endif
    }

    /* Anything else is a label from core/partition_manager's user area
     * (the "bparted" command): the root "littlefs" is already mounted at
     * "/" and "swap" isn't a filesystem, so storage__mount_partition()
     * rejects both; any other label is an extra partition to mount here. */
    if (!disk__mount_point_valid(mount_point)) return BRUCE_ERR_INVALID_PATH;
    return storage__mount_partition(name, mount_point);
}

/* Boot-time counterpart to disk__mount("sd0", "/sdcard"): main.c calls this
 * before any process is registered, so disk__mount()'s "built-in caller"
 * permission check would reject it (process_registry__current_context()
 * finds no record for the main task at all). A card that fails to mount
 * here - absent, unformatted, wrong voltage - is routine, not fatal, so
 * this reports success only, matching storage__sd_mount_spi()'s own
 * warning-level logging on failure. Returns false immediately, without
 * touching hardware, when the board has no SD slot at all. */
bool disk__mount_sd_boot(void) {
#if CONFIG_BRUCE_SD_ENABLED
    return disk__sd_mount_default();
#else
    return false;
#endif
}

bruce_result_t disk__unmount(const char *name_or_mount_point) {
    if (name_or_mount_point == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    if (!disk__caller_is_built_in()) return BRUCE_ERR_PERMISSION;
    if (strcmp(name_or_mount_point, "sd0") == 0 || strcmp(name_or_mount_point, "/sdcard") == 0) {
        return storage__sd_unmount();
    }
    return storage__unmount_partition(name_or_mount_point);
}
