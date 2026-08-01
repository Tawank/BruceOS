#pragma once

/* Core-private VFS/storage bootstrap.  Process-owned opaque file handles are
 * declared in the public <bruce/storage.h> SDK header. */

#include <stdbool.h>
#include <stddef.h>

#include "core_sdk/storage.h" // IWYU pragma: export

/* All paths are VFS paths: internal files use /..., SD files use /sdcard/....
 * An empty mount path registers LittleFS as the fallback VFS (root filesystem).
 * A single "/" is not a valid VFS prefix because it is shorter than two chars. */
#define STORAGE__MOUNT_PATH ""
#define STORAGE__SD_MOUNT_PATH "/sdcard"

typedef struct {
    int host;
    int mosi_gpio;
    int miso_gpio;
    int sck_gpio;
    int cs_gpio;
} storage__sdspi_config_t;

bool storage__init(void);
bool storage__exists_internal(const char *path);
bool storage__read_file(const char *path, char **data, size_t *size);
bool storage__write_file_atomic(const char *path, const void *data, size_t size);
bool storage__remove_internal(const char *path);
bool storage__rename_internal(const char *from, const char *to);
bool storage__get_usage_internal(const char *path, size_t *total_bytes, size_t *used_bytes);

/* SD is optional and board-specific. The caller supplies the SPI host and pins. */
bool storage__sd_mount_spi(const storage__sdspi_config_t *config);
void storage__sd_unmount(void);
bool storage__sd_is_ready(void);

void storage__free(void *data);
