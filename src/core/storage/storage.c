#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dirent.h>
#include <errno.h> // IWYU pragma: export
#include <fcntl.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "spi_flash_mmap.h"

#include "core/partition_manager/partition_manager.h"
#include "core/process/process.h"
#include "core_sdk/permission.h"
#include "core_sdk/storage.h"

#define STORAGE__PATH_MAX 192

static const char *const TAG = "bruce_storage";
static StaticSemaphore_t s_storage_mutex_storage;
static SemaphoreHandle_t s_storage_mutex;
static portMUX_TYPE s_storage_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_ready;
static bool s_sd_ready;
static bool s_sd_bus_owned;
static int s_sd_host;
static sdmmc_card_t *s_sd_card;
static const esp_partition_t *s_littlefs_partition;

/* Extra internal LittleFS partitions mounted via storage__mount_partition()
 * (core/partition_manager entries besides the root "littlefs" and "swap").
 * At most BRUCE_PARTITION_MAX_ENTRIES can ever exist, root and swap included,
 * so that bounds this table too. label/mount_point are heap-allocated: they
 * are set once per mount and freed on unmount, so there is no point paying
 * for BRUCE_PARTITION_LABEL_MAX/STORAGE__PATH_MAX of static storage per slot. */
typedef struct {
    bool in_use;
    char *label;
    char *mount_point;
} storage__extra_mount_t;

static storage__extra_mount_t s_extra_mounts[BRUCE_PARTITION_MAX_ENTRIES];

static bool storage__is_protected_path(const char *path);
static bool storage__is_valid_public_path(const char *path);
static bool storage__is_sd_path(const char *path);
static bool storage__has_open_sd_files_locked(void);
static int storage__extra_mount_find_by_label_locked(const char *label);
static int storage__extra_mount_find_by_point_locked(const char *mount_point);
static int storage__extra_mount_index_for_path_locked(const char *path);
static bool storage__extra_mount_has_open_files_locked(int index);

static void storage__lock(void) {
    if (s_storage_mutex == NULL) {
        portENTER_CRITICAL(&s_storage_init_mux);
        if (s_storage_mutex == NULL) s_storage_mutex = xSemaphoreCreateMutexStatic(&s_storage_mutex_storage);
        portEXIT_CRITICAL(&s_storage_init_mux);
    }
    xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
}

static void storage__unlock(void) { xSemaphoreGive(s_storage_mutex); }

static char *storage__strdup(const char *value) {
    size_t length = strlen(value) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, value, length);
    return copy;
}

/* VFS registrations make a mount path directly addressable, but the root
 * fallback filesystem only enumerates mount points that also exist as real
 * directories. Create that backing entry before registering the VFS so
 * storage__list("/") (and therefore ls/Filemanager) can discover it. */
static bool storage__ensure_mount_directory_locked(const char *path, bool *out_created) {
    if (out_created != NULL) *out_created = false;
    struct stat path_stat;
    if (stat(path, &path_stat) == 0) return S_ISDIR(path_stat.st_mode);
    if (errno != ENOENT || mkdir(path, 0775) != 0) return false;
    if (out_created != NULL) *out_created = true;
    return true;
}

static bool storage__littlefs_metadata_erased(const esp_partition_t *partition) {
    uint8_t buffer[256];
    size_t check_size = partition->size < 2 * SPI_FLASH_SEC_SIZE ? partition->size : 2 * SPI_FLASH_SEC_SIZE;
    for (size_t offset = 0; offset < check_size; offset += sizeof(buffer)) {
        size_t chunk = check_size - offset < sizeof(buffer) ? check_size - offset : sizeof(buffer);
        if (esp_partition_read(partition, offset, buffer, chunk) != ESP_OK) return false;
        for (size_t i = 0; i < chunk; ++i) {
            if (buffer[i] != 0xff) return false;
        }
    }
    return true;
}

static esp_err_t storage__mount_internal(void) {
    /* core/partition_manager owns everything after the static partitions in
     * partitions.csv: by default (no user partition table ever committed)
     * that's the exact legacy layout below - one littlefs partition
     * spanning all remaining flash - but a committed table can carve that
     * space into a "swap" partition, additional labeled littlefs volumes,
     * etc. Either way it hands back the entry labeled "littlefs" already
     * registered as an esp_partition_t, ready to mount exactly as before. */
    if (partition_manager__init_for_storage(&s_littlefs_partition) != BRUCE_OK ||
        s_littlefs_partition == NULL) {
        return ESP_FAIL;
    }

    const esp_vfs_littlefs_conf_t config = {
        .base_path = STORAGE__MOUNT_PATH,
        .partition_label = NULL,
        .partition = s_littlefs_partition,
#ifdef CONFIG_BRUCE_QEMU_TEST_MODE
        /* QEMU's offline flash image does not preserve erased bytes in the
         * dynamically registered root volume. It must be formatted on first
         * boot, unlike a physical device where mount corruption is retained. */
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .grow_on_mount = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err != ESP_OK && storage__littlefs_metadata_erased(s_littlefs_partition)) {
        ESP_LOGI(TAG, "formatting empty internal storage");
        err = esp_littlefs_format_partition(s_littlefs_partition);
        if (err == ESP_OK) err = esp_vfs_littlefs_register(&config);
    }
    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "mounted internal storage at 0x%zx (%zu bytes)",
            (size_t)s_littlefs_partition->address,
            s_littlefs_partition->size
        );
    }
    return err;
}

bool storage__init(void) {
    storage__lock();
    if (!s_initialized) {
        esp_err_t err = storage__mount_internal();
        s_ready = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
        s_initialized = true;
        if (!s_ready) ESP_LOGE(TAG, "could not mount internal storage: %s", esp_err_to_name(err));
    }
    bool ready = s_ready;
    storage__unlock();
    return ready;
}

bool storage__mkdir_internal(const char *path) {
    if (path == NULL || path[0] != '/') return false;
    /* stat()/mkdir() below can block on flash I/O; a force-kill must not
     * delete this task while it holds s_storage_mutex mid-syscall, or every
     * later storage__* call would deadlock on the now-unreleasable mutex.
     * operation_begin/end makes process__kill() wait for us to finish first
     * (same pattern as audio__play() in core/audio/audio.c). */
    if (!process_registry__operation_begin()) return false;
    storage__lock();
    struct stat path_stat;
    bool created =
        s_ready && ((stat(path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) || mkdir(path, 0775) == 0);
    storage__unlock();
    process_registry__operation_end();
    return created;
}

static bool storage__is_sd_path(const char *path) {
    size_t mount_length = strlen(STORAGE__SD_MOUNT_PATH);
    return path != NULL && strncmp(path, STORAGE__SD_MOUNT_PATH, mount_length) == 0 &&
           (path[mount_length] == '\0' || path[mount_length] == '/');
}

static bool storage__is_ready(const char *path) {
    if (path == NULL) return false;
    if (storage__is_sd_path(path)) return s_sd_ready;
    if (storage__extra_mount_index_for_path_locked(path) >= 0) return true;
    return s_ready;
}

static int storage__extra_mount_find_by_label_locked(const char *label) {
    if (label == NULL) return -1;
    for (int i = 0; i < (int)BRUCE_PARTITION_MAX_ENTRIES; ++i) {
        if (s_extra_mounts[i].in_use && strcmp(s_extra_mounts[i].label, label) == 0) return i;
    }
    return -1;
}

static int storage__extra_mount_find_by_point_locked(const char *mount_point) {
    if (mount_point == NULL) return -1;
    for (int i = 0; i < (int)BRUCE_PARTITION_MAX_ENTRIES; ++i) {
        if (s_extra_mounts[i].in_use && strcmp(s_extra_mounts[i].mount_point, mount_point) == 0) return i;
    }
    return -1;
}

/* Longest-prefix match at a path-component boundary, mirroring
 * storage__is_sd_path()'s rule so a path under an extra mount is recognized
 * the same way one under /sdcard is. */
static int storage__extra_mount_index_for_path_locked(const char *path) {
    if (path == NULL) return -1;
    for (int i = 0; i < (int)BRUCE_PARTITION_MAX_ENTRIES; ++i) {
        if (!s_extra_mounts[i].in_use) continue;
        size_t mount_length = strlen(s_extra_mounts[i].mount_point);
        if (strncmp(path, s_extra_mounts[i].mount_point, mount_length) == 0 &&
            (path[mount_length] == '\0' || path[mount_length] == '/')) {
            return i;
        }
    }
    return -1;
}

static bool storage__read_file_locked(const char *path, char **data, size_t *size) {
    if (data == NULL || size == NULL || !storage__is_ready(path)) return false;
    *data = NULL;
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    char *buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        return false;
    }
    size_t read = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(buffer);
        return false;
    }
    buffer[length] = '\0';
    *data = buffer;
    *size = (size_t)length;
    return true;
}

static bool storage__write_file_atomic_locked(const char *path, const void *data, size_t size) {
    char temporary_path[STORAGE__PATH_MAX];
    if ((data == NULL && size != 0) || !storage__is_ready(path) ||
        snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", path) >= (int)sizeof(temporary_path))
        return false;
    FILE *file = fopen(temporary_path, "wb");
    if (file == NULL) return false;
    const void *content = data != NULL ? data : "";
    bool written = fwrite(content, 1, size, file) == size && fflush(file) == 0;
    fclose(file);
    if (!written) {
        remove(temporary_path);
        return false;
    }
    remove(path);
    if (rename(temporary_path, path) != 0) {
        remove(temporary_path);
        return false;
    }
    return true;
}

bool storage__exists_internal(const char *path) {
    storage__lock();
    struct stat path_stat;
    bool exists = storage__is_ready(path) && stat(path, &path_stat) == 0;
    storage__unlock();
    return exists;
}

bruce_result_t storage__exists(const char *path, bool *out_exists) {
    if (out_exists == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_exists = false;
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_STORAGE);
    if (permission != BRUCE_OK) return permission;
    if (!storage__is_valid_public_path(path)) return BRUCE_ERR_INVALID_PATH;
    if (storage__is_protected_path(path)) return BRUCE_ERR_PERMISSION;

    storage__lock();
    if (!storage__is_ready(path)) {
        storage__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    struct stat path_stat;
    int status = stat(path, &path_stat);
    if (status == 0) *out_exists = true;
    bruce_result_t result = status == 0 || errno == ENOENT ? BRUCE_OK : BRUCE_ERR_IO;
    storage__unlock();
    return result;
}

bool storage__read_file(const char *path, char **data, size_t *size) {
    storage__lock();
    bool read = storage__read_file_locked(path, data, size);
    storage__unlock();
    return read;
}

bool storage__write_file_atomic(const char *path, const void *data, size_t size) {
    storage__lock();
    bool written = storage__write_file_atomic_locked(path, data, size);
    storage__unlock();
    return written;
}

bool storage__remove_internal(const char *path) {
    storage__lock();
    struct stat path_stat;
    bool removed = storage__is_ready(path) && stat(path, &path_stat) == 0 &&
                   (S_ISDIR(path_stat.st_mode) ? rmdir(path) : remove(path)) == 0;
    storage__unlock();
    return removed;
}

bool storage__rename_internal(const char *from, const char *to) {
    storage__lock();
    bool renamed = storage__is_ready(from) && storage__is_ready(to) &&
                   storage__is_sd_path(from) == storage__is_sd_path(to) && rename(from, to) == 0;
    storage__unlock();
    return renamed;
}

bool storage__get_usage_internal(const char *path, size_t *total_bytes, size_t *used_bytes) {
    storage__lock();
    bool known = false;
    if (total_bytes != NULL && used_bytes != NULL && path != NULL) {
        int extra_index = storage__extra_mount_index_for_path_locked(path);
        if (storage__is_sd_path(path) && s_sd_ready) {
            uint64_t total = 0;
            uint64_t free_bytes = 0;
            if (esp_vfs_fat_info(STORAGE__SD_MOUNT_PATH, &total, &free_bytes) == ESP_OK) {
                *total_bytes = (size_t)total;
                *used_bytes = (size_t)(total - free_bytes);
                known = true;
            }
        } else if (extra_index >= 0) {
            known = esp_littlefs_info(s_extra_mounts[extra_index].label, total_bytes, used_bytes) == ESP_OK;
        } else if (!storage__is_sd_path(path) && s_ready) {
            known = esp_littlefs_partition_info(s_littlefs_partition, total_bytes, used_bytes) == ESP_OK;
        }
    }
    storage__unlock();
    return known;
}

bool storage__sd_mount_spi(const storage__sdspi_config_t *config) {
    if (config == NULL || config->mosi_gpio < 0 || config->miso_gpio < 0 || config->sck_gpio < 0 ||
        config->cs_gpio < 0)
        return false;
    storage__lock();
    if (s_sd_ready) {
        storage__unlock();
        return true;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = config->miso_gpio,
        .sclk_io_num = config->sck_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(config->host, &bus, SPI_DMA_CH_AUTO);
    s_sd_bus_owned = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "could not initialize SD SPI bus: %s", esp_err_to_name(err));
        storage__unlock();
        return false;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = config->host;
    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.gpio_cs = config->cs_gpio;
    device.host_id = host.slot;
    bool mount_directory_created = false;
    if (!storage__ensure_mount_directory_locked(STORAGE__SD_MOUNT_PATH, &mount_directory_created)) {
        if (s_sd_bus_owned) spi_bus_free(config->host);
        s_sd_bus_owned = false;
        storage__unlock();
        return false;
    }
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(STORAGE__SD_MOUNT_PATH, &host, &device, &mount, &s_sd_card);
    if (err != ESP_OK) {
        if (s_sd_bus_owned) spi_bus_free(config->host);
        s_sd_bus_owned = false;
        if (mount_directory_created) (void)rmdir(STORAGE__SD_MOUNT_PATH);
        ESP_LOGW(TAG, "could not mount SD card: %s", esp_err_to_name(err));
        storage__unlock();
        return false;
    }
    s_sd_host = config->host;
    s_sd_ready = true;
    ESP_LOGI(
        TAG,
        "mounted SD card at %s (%llu bytes)",
        STORAGE__SD_MOUNT_PATH,
        (unsigned long long)s_sd_card->csd.capacity * s_sd_card->csd.sector_size
    );
    storage__unlock();
    return true;
}

bruce_result_t storage__sd_unmount(void) {
    storage__lock();
    if (!s_sd_ready) {
        storage__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    if (storage__has_open_sd_files_locked()) {
        storage__unlock();
        return BRUCE_ERR_BUSY;
    }
    if (esp_vfs_fat_sdcard_unmount(STORAGE__SD_MOUNT_PATH, s_sd_card) != ESP_OK) {
        storage__unlock();
        return BRUCE_ERR_IO;
    }
    if (s_sd_bus_owned) spi_bus_free(s_sd_host);
    s_sd_card = NULL;
    s_sd_ready = false;
    s_sd_bus_owned = false;
    storage__unlock();
    return BRUCE_OK;
}

bool storage__sd_is_ready(void) {
    storage__lock();
    bool ready = s_sd_ready;
    storage__unlock();
    return ready;
}

bool storage__get_sd_capacity(uint64_t *out_size) {
    if (out_size == NULL) return false;
    storage__lock();
    bool ready = s_sd_ready && s_sd_card != NULL;
    if (ready) *out_size = (uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size;
    storage__unlock();
    return ready;
}

bool storage__is_internal_partition_mounted(const char *label) {
    storage__lock();
    bool mounted = (s_ready && s_littlefs_partition != NULL && label != NULL &&
                     strcmp(s_littlefs_partition->label, label) == 0) ||
                   storage__extra_mount_find_by_label_locked(label) >= 0;
    storage__unlock();
    return mounted;
}

bool storage__internal_mount_point(const char *label, char *out, size_t capacity) {
    if (label == NULL || out == NULL || capacity == 0) return false;
    storage__lock();
    const char *point = NULL;
    if (s_ready && s_littlefs_partition != NULL && strcmp(s_littlefs_partition->label, label) == 0) {
        point = "/";
    } else {
        int index = storage__extra_mount_find_by_label_locked(label);
        if (index >= 0) point = s_extra_mounts[index].mount_point;
    }
    if (point != NULL) snprintf(out, capacity, "%s", point);
    storage__unlock();
    return point != NULL;
}

/* core/partition_manager already formats a newly-created entry the boot it
 * first registers it (see partition_manager__register_and_apply()), so the
 * mount below normally just succeeds; the erased-metadata fallback mirrors
 * storage__mount_internal()'s and only matters if that format was missed. */
bruce_result_t storage__mount_partition(const char *label, const char *mount_point) {
    if (label == NULL || mount_point == NULL || mount_point[0] != '/') return BRUCE_ERR_INVALID_ARGUMENT;
    size_t mount_point_length = strlen(mount_point);
    if (mount_point_length >= STORAGE__PATH_MAX) return BRUCE_ERR_INVALID_ARGUMENT;

    storage__lock();
    if (!s_ready) {
        storage__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    bool label_taken = (s_littlefs_partition != NULL && strcmp(s_littlefs_partition->label, label) == 0) ||
                        storage__extra_mount_find_by_label_locked(label) >= 0;
    bool point_taken = strcmp(mount_point, "/") == 0 || storage__is_sd_path(mount_point) ||
                        storage__extra_mount_find_by_point_locked(mount_point) >= 0;
    if (label_taken || point_taken) {
        storage__unlock();
        return BRUCE_ERR_ALREADY_EXISTS;
    }
    int slot = -1;
    for (int i = 0; i < (int)BRUCE_PARTITION_MAX_ENTRIES; ++i) {
        if (!s_extra_mounts[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        storage__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, label);
    if (partition == NULL) {
        storage__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }

    /* Copied before touching the VFS so a failed allocation never leaves a
     * mount registered with nothing in s_extra_mounts to unregister it. */
    char *label_copy = storage__strdup(label);
    char *mount_point_copy = label_copy != NULL ? storage__strdup(mount_point) : NULL;
    if (label_copy == NULL || mount_point_copy == NULL) {
        free(label_copy);
        free(mount_point_copy);
        storage__unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    bool mount_directory_created = false;
    if (!storage__ensure_mount_directory_locked(mount_point, &mount_directory_created)) {
        free(label_copy);
        free(mount_point_copy);
        storage__unlock();
        return BRUCE_ERR_INVALID_PATH;
    }

    const esp_vfs_littlefs_conf_t config = {
        .base_path = mount_point,
        .partition_label = NULL,
        .partition = partition,
        .format_if_mount_failed = false,
        .grow_on_mount = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err != ESP_OK && storage__littlefs_metadata_erased(partition)) {
        ESP_LOGI(TAG, "formatting empty partition '%s'", label);
        err = esp_littlefs_format_partition(partition);
        if (err == ESP_OK) err = esp_vfs_littlefs_register(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not mount '%s' at %s: %s", label, mount_point, esp_err_to_name(err));
        if (mount_directory_created) (void)rmdir(mount_point);
        free(label_copy);
        free(mount_point_copy);
        storage__unlock();
        return BRUCE_ERR_IO;
    }

    s_extra_mounts[slot].in_use = true;
    s_extra_mounts[slot].label = label_copy;
    s_extra_mounts[slot].mount_point = mount_point_copy;
    storage__unlock();
    return BRUCE_OK;
}

bruce_result_t storage__unmount_partition(const char *label_or_mount_point) {
    if (label_or_mount_point == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    storage__lock();
    int index = storage__extra_mount_find_by_label_locked(label_or_mount_point);
    if (index < 0) index = storage__extra_mount_find_by_point_locked(label_or_mount_point);
    if (index < 0) {
        storage__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (storage__extra_mount_has_open_files_locked(index)) {
        storage__unlock();
        return BRUCE_ERR_BUSY;
    }
    esp_err_t err = esp_vfs_littlefs_unregister(s_extra_mounts[index].label);
    if (err != ESP_OK) {
        storage__unlock();
        return BRUCE_ERR_IO;
    }
    free(s_extra_mounts[index].label);
    free(s_extra_mounts[index].mount_point);
    s_extra_mounts[index].label = NULL;
    s_extra_mounts[index].mount_point = NULL;
    s_extra_mounts[index].in_use = false;
    storage__unlock();
    return BRUCE_OK;
}

void storage__free(void *data) { free(data); }

/* ------------------------------------------------------------------------ */
/* A5: process-owned opaque file handles (core_sdk/storage.h)                  */
/* ------------------------------------------------------------------------ */

#define STORAGE__MAX_OPEN_FILES 16

typedef struct {
    bool in_use;
    bool sd;
    int extra_mount; /* Index into s_extra_mounts, or -1 when not under one. */
    int fd;
    bruce_file_id_t id;
    bruce_resource_id_t resource_id;
    bruce_process_id_t owner;
    TaskHandle_t service_owner;
} storage__file_slot_t;

static storage__file_slot_t s_open_files[STORAGE__MAX_OPEN_FILES];
static uint32_t s_next_file_id = 1;

static bool storage__has_open_sd_files_locked(void) {
    for (int i = 0; i < STORAGE__MAX_OPEN_FILES; ++i) {
        if (s_open_files[i].in_use && s_open_files[i].sd) return true;
    }
    return false;
}

static bool storage__extra_mount_has_open_files_locked(int index) {
    for (int i = 0; i < STORAGE__MAX_OPEN_FILES; ++i) {
        if (s_open_files[i].in_use && s_open_files[i].extra_mount == index) return true;
    }
    return false;
}

/* The permission store and its atomic-write sibling are not accessible through
 * the public storage API. Everything else mounted (LittleFS or SD), including
 * the user-editable Bruce configuration, is reachable by a storage-granted
 * caller. Core still reads/writes the permission store directly through
 * storage__read_file()/storage__write_file_atomic() above. */
static bool storage__is_protected_path(const char *path) {
    static const char *const protected_paths[] = {
        "/config/permissions.json",
        "/config/permissions.json.tmp",
    };
    if (path == NULL) return false;
    for (size_t i = 0; i < sizeof(protected_paths) / sizeof(protected_paths[0]); ++i) {
        if (strcmp(path, protected_paths[i]) == 0) return true;
    }
    return false;
}

static bool storage__is_valid_public_path(const char *path) {
    if (path == NULL || path[0] != '/' || strlen(path) >= BRUCE_STORAGE_PATH_MAX) return false;
    const char *component = path + 1;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t length = end != NULL ? (size_t)(end - component) : strlen(component);
        if (length == 0 || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (end == NULL) break;
        component = end + 1;
    }
    return true;
}

/* Caller must hold s_storage_mutex. */
static int storage__find_open_slot_locked(bruce_file_id_t file) {
    if (file == BRUCE_FILE_ID_INVALID) return -1;
    for (int i = 0; i < STORAGE__MAX_OPEN_FILES; ++i) {
        if (s_open_files[i].in_use && s_open_files[i].id == file) return i;
    }
    return -1;
}

/* Core service tasks are not process-registry entries. Their handles remain
 * task-owned, but require explicit close rather than process-exit cleanup. */
static bool storage__file_owned_by_caller_locked(const storage__file_slot_t *slot) {
    bruce_process_id_t process = process__current_id();
    return slot->owner != BRUCE_PROCESS_ID_INVALID ? slot->owner == process
                                                   : slot->service_owner == xTaskGetCurrentTaskHandle();
}

/* Invoked by process_registry__* teardown if a process exits/is killed without
 * closing its own handles; runs while the process registry's own lock is held,
 * so it must not call back into process_registry__* itself (mirrors
 * memory__cleanup's rule in core/memory/memory.c). */
static void storage__file_cleanup(void *context) {
    storage__file_slot_t *slot = (storage__file_slot_t *)context;
    storage__lock();
    if (slot->in_use) {
        close(slot->fd);
        slot->in_use = false;
        slot->id = BRUCE_FILE_ID_INVALID;
    }
    storage__unlock();
}

bruce_result_t storage__open(const char *path, uint32_t flags, bruce_file_id_t *out_file) {
    if (out_file == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_file = BRUCE_FILE_ID_INVALID;

    bruce_result_t permission = permission__check(BRUCE_PERMISSION_STORAGE);
    if (permission != BRUCE_OK) return permission;
    if (!storage__is_valid_public_path(path)) return BRUCE_ERR_INVALID_PATH;
    if (storage__is_protected_path(path)) return BRUCE_ERR_PERMISSION;

    bool want_read = (flags & BRUCE_STORAGE_OPEN_READ) != 0;
    bool want_write = (flags & BRUCE_STORAGE_OPEN_WRITE) != 0;
    bool want_append = (flags & BRUCE_STORAGE_OPEN_APPEND) != 0;
    bool want_create = (flags & BRUCE_STORAGE_OPEN_CREATE) != 0;
    bool want_truncate = (flags & BRUCE_STORAGE_OPEN_TRUNCATE) != 0;
    if (!want_read && !want_write && !want_append) return BRUCE_ERR_INVALID_ARGUMENT;

    int posix_flags;
    if (want_append) {
        posix_flags = (want_read ? O_RDWR : O_WRONLY) | O_APPEND;
    } else if (want_read && want_write) {
        posix_flags = O_RDWR;
    } else if (want_write) {
        posix_flags = O_WRONLY;
    } else {
        posix_flags = O_RDONLY;
    }
    if (want_create) posix_flags |= O_CREAT;
    if (want_truncate) posix_flags |= O_TRUNC;

    storage__lock();
    if (!storage__is_ready(path)) {
        storage__unlock();
        return BRUCE_ERR_IO;
    }
    int slot_index = -1;
    for (int i = 0; i < STORAGE__MAX_OPEN_FILES; ++i) {
        if (!s_open_files[i].in_use) {
            slot_index = i;
            break;
        }
    }
    if (slot_index < 0) {
        storage__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    /* Reserve the slot before releasing the lock: open() and
     * process_registry__resource_register() must never run while
     * s_storage_mutex is held. The cleanup callback above re-enters
     * s_storage_mutex from inside the process registry's own lock during process
     * teardown, so nesting the two locks in the opposite order here would
     * be a lock-order inversion. */
    s_open_files[slot_index].in_use = true;
    s_open_files[slot_index].sd = storage__is_sd_path(path);
    s_open_files[slot_index].extra_mount = storage__extra_mount_index_for_path_locked(path);
    s_open_files[slot_index].id = BRUCE_FILE_ID_INVALID;
    s_open_files[slot_index].fd = -1;
    storage__unlock();

    int fd = open(path, posix_flags, 0644);
    if (fd < 0) {
        storage__lock();
        s_open_files[slot_index].in_use = false;
        storage__unlock();
        return errno == ENOENT ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_IO;
    }

    bruce_process_id_t owner = process__current_id();
    bruce_resource_id_t resource_id = BRUCE_RESOURCE_ID_INVALID;
    if (owner != BRUCE_PROCESS_ID_INVALID) {
        resource_id = process_registry__resource_register(storage__file_cleanup, &s_open_files[slot_index]);
    }
    if (owner != BRUCE_PROCESS_ID_INVALID && resource_id == BRUCE_RESOURCE_ID_INVALID) {
        close(fd);
        storage__lock();
        s_open_files[slot_index].in_use = false;
        storage__unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    storage__lock();
    s_open_files[slot_index].fd = fd;
    s_open_files[slot_index].resource_id = resource_id;
    s_open_files[slot_index].owner = owner;
    s_open_files[slot_index].service_owner =
        owner == BRUCE_PROCESS_ID_INVALID ? xTaskGetCurrentTaskHandle() : NULL;
    bruce_file_id_t id = s_next_file_id++;
    if (s_next_file_id == BRUCE_FILE_ID_INVALID) s_next_file_id = 1;
    s_open_files[slot_index].id = id;
    *out_file = id;
    storage__unlock();
    return BRUCE_OK;
}

bruce_result_t storage__read(bruce_file_id_t file, void *buffer, size_t capacity, size_t *out_size) {
    if (buffer == NULL || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    storage__lock();
    int slot_index = storage__find_open_slot_locked(file);
    if (slot_index < 0) {
        storage__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (!storage__file_owned_by_caller_locked(&s_open_files[slot_index])) {
        storage__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    ssize_t result = read(s_open_files[slot_index].fd, buffer, capacity);
    storage__unlock();
    if (result < 0) return BRUCE_ERR_IO;
    *out_size = (size_t)result;
    return BRUCE_OK;
}

bruce_result_t storage__write(bruce_file_id_t file, const void *buffer, size_t size, size_t *out_size) {
    if (out_size == NULL || (buffer == NULL && size != 0)) return BRUCE_ERR_INVALID_ARGUMENT;
    storage__lock();
    int slot_index = storage__find_open_slot_locked(file);
    if (slot_index < 0) {
        storage__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (!storage__file_owned_by_caller_locked(&s_open_files[slot_index])) {
        storage__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    ssize_t result = write(s_open_files[slot_index].fd, buffer, size);
    storage__unlock();
    if (result < 0) return BRUCE_ERR_IO;
    *out_size = (size_t)result;
    return BRUCE_OK;
}

bruce_result_t storage__seek(bruce_file_id_t file, int64_t offset, int whence, uint64_t *out_position) {
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return BRUCE_ERR_INVALID_ARGUMENT;
    storage__lock();
    int slot_index = storage__find_open_slot_locked(file);
    if (slot_index < 0) {
        storage__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (!storage__file_owned_by_caller_locked(&s_open_files[slot_index])) {
        storage__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    off_t result = lseek(s_open_files[slot_index].fd, (off_t)offset, whence);
    storage__unlock();
    if (result < 0) return BRUCE_ERR_IO;
    if (out_position != NULL) *out_position = (uint64_t)result;
    return BRUCE_OK;
}

bruce_result_t storage__close(bruce_file_id_t file) {
    storage__lock();
    int slot_index = storage__find_open_slot_locked(file);
    if (slot_index < 0) {
        storage__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (!storage__file_owned_by_caller_locked(&s_open_files[slot_index])) {
        storage__unlock();
        return BRUCE_ERR_PERMISSION;
    }
    int fd = s_open_files[slot_index].fd;
    bruce_resource_id_t resource_id = s_open_files[slot_index].resource_id;
    s_open_files[slot_index].in_use = false;
    s_open_files[slot_index].id = BRUCE_FILE_ID_INVALID;
    storage__unlock();

    close(fd);
    /* The handle is already released above; this only removes the now-
     * redundant teardown-time cleanup registration, it does not close fd
     * again. */
    if (resource_id != BRUCE_RESOURCE_ID_INVALID) process_registry__resource_release(resource_id);
    return BRUCE_OK;
}

bruce_result_t
storage__list(const char *path, bruce_storage_entry_t *entries, size_t capacity, size_t *out_count) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_STORAGE);
    if (permission != BRUCE_OK) return permission;
    if (out_count == NULL || !storage__is_valid_public_path(path) || (capacity != 0 && entries == NULL)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    storage__lock();
    if (!storage__is_ready(path)) {
        storage__unlock();
        return BRUCE_ERR_IO;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        storage__unlock();
        return errno == ENOENT ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_IO;
    }

    size_t path_length = strlen(path);
    bool has_trailing_slash = path_length > 0 && path[path_length - 1] == '/';
    size_t written = 0;
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir)) != NULL) {
        if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0) continue;

        char full_path[BRUCE_STORAGE_PATH_MAX];
        int printed = snprintf(
            full_path, sizeof(full_path), has_trailing_slash ? "%s%s" : "%s/%s", path, dir_entry->d_name
        );
        if (printed < 0 || (size_t)printed >= sizeof(full_path)) continue;
        if (storage__is_protected_path(full_path)) continue;

        struct stat entry_stat;
        if (stat(full_path, &entry_stat) != 0) continue;

        if (entries != NULL && written < capacity) {
            bruce_storage_entry_t *out = &entries[written];
            strncpy(out->name, dir_entry->d_name, BRUCE_STORAGE_NAME_MAX - 1);
            out->name[BRUCE_STORAGE_NAME_MAX - 1] = '\0';
            out->type =
                S_ISDIR(entry_stat.st_mode) ? BRUCE_STORAGE_ENTRY_DIRECTORY : BRUCE_STORAGE_ENTRY_FILE;
            out->size = (size_t)entry_stat.st_size;
        }
        ++written;
    }
    closedir(dir);
    *out_count = written;
    storage__unlock();
    return BRUCE_OK;
}

bruce_result_t storage__mkdir(const char *path) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_STORAGE);
    if (permission != BRUCE_OK) return permission;
    if (!storage__is_valid_public_path(path) || strcmp(path, "/") == 0) return BRUCE_ERR_INVALID_PATH;
    if (storage__is_protected_path(path)) return BRUCE_ERR_PERMISSION;

    storage__lock();
    bruce_result_t result = BRUCE_OK;
    struct stat path_stat;
    if (!storage__is_ready(path)) result = BRUCE_ERR_INVALID_STATE;
    else if (stat(path, &path_stat) == 0)
        result = S_ISDIR(path_stat.st_mode) ? BRUCE_OK : BRUCE_ERR_INVALID_PATH;
    else if (mkdir(path, 0775) != 0) result = errno == ENOENT ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_IO;
    storage__unlock();
    return result;
}

bruce_result_t storage__remove(const char *path) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_STORAGE);
    if (permission != BRUCE_OK) return permission;
    if (!storage__is_valid_public_path(path) || strcmp(path, "/") == 0) return BRUCE_ERR_INVALID_PATH;
    if (storage__is_protected_path(path)) return BRUCE_ERR_PERMISSION;

    storage__lock();
    if (!storage__is_ready(path)) {
        storage__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    struct stat path_stat;
    if (stat(path, &path_stat) != 0) {
        storage__unlock();
        return errno == ENOENT ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_IO;
    }
    int removed = S_ISDIR(path_stat.st_mode) ? rmdir(path) : remove(path);
    bruce_result_t result = removed == 0 ? BRUCE_OK : errno == ENOTEMPTY ? BRUCE_ERR_BUSY : BRUCE_ERR_IO;
    storage__unlock();
    return result;
}

bruce_result_t storage__rename(const char *from, const char *to) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_STORAGE);
    if (permission != BRUCE_OK) return permission;
    if (!storage__is_valid_public_path(from) || !storage__is_valid_public_path(to) ||
        strcmp(from, "/") == 0 || strcmp(to, "/") == 0) {
        return BRUCE_ERR_INVALID_PATH;
    }
    if (storage__is_protected_path(from) || storage__is_protected_path(to)) return BRUCE_ERR_PERMISSION;
    if (storage__is_sd_path(from) != storage__is_sd_path(to)) return BRUCE_ERR_INVALID_ARGUMENT;

    storage__lock();
    bruce_result_t result = BRUCE_OK;
    struct stat path_stat;
    if (!storage__is_ready(from) || !storage__is_ready(to)) result = BRUCE_ERR_INVALID_STATE;
    else if (stat(from, &path_stat) != 0) result = errno == ENOENT ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_IO;
    else if (stat(to, &path_stat) == 0) result = BRUCE_ERR_ALREADY_EXISTS;
    else if (rename(from, to) != 0) result = errno == ENOENT ? BRUCE_ERR_NOT_FOUND : BRUCE_ERR_IO;
    storage__unlock();
    return result;
}

bruce_result_t storage__get_usage(const char *path, size_t *total_bytes, size_t *used_bytes) {
    bruce_result_t permission = permission__check(BRUCE_PERMISSION_STORAGE);
    if (permission != BRUCE_OK) return permission;
    if (!storage__is_valid_public_path(path) || total_bytes == NULL || used_bytes == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    return storage__get_usage_internal(path, total_bytes, used_bytes) ? BRUCE_OK : BRUCE_ERR_INVALID_STATE;
}
