#include "lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ext_mem_loader/ext_mem_loader_elf.h"
#include "core/process/process.h"
#include "core/storage/storage.h"
#include "core_sdk/ext_mem_loader.h"
#include "esp_elf.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "private/elf_symbol.h"
#include "sdkconfig.h"

#define LIB__NAME_MAX 24
#define LIB__PATH_MAX 48

static const char *TAG = "lib";

extern const struct esp_elfsym g_bruce_lib_elfsyms[];

/* A loaded library. Without executable PSRAM its code runs from swap, which
 * belongs to the process that loaded it, so only that process may use it. */
typedef struct lib__entry {
    struct lib__entry *next;
    char name[LIB__NAME_MAX];
    esp_elf_t elf;
    bruce_ext_mem_loader_xip_image_t xip;
    bool in_swap;
    bruce_process_id_t owner;
    const void *api;
    int users;
} lib__entry_t;

/* One lib__open(): a resource of the calling process, so a process that dies
 * without lib__close() still drops its use. */
struct lib__handle {
    lib__entry_t *entry;
    bruce_resource_id_t resource;
};

/* s_list_lock guards s_libs and the use counts; it is never held across a
 * call into the process registry, since process teardown runs lib__cleanup()
 * with the registry locked. s_load_lock serializes loads. */
static SemaphoreHandle_t s_list_lock;
static SemaphoreHandle_t s_load_lock;
static StaticSemaphore_t s_list_lock_storage;
static StaticSemaphore_t s_load_lock_storage;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static lib__entry_t *s_libs;
static TaskHandle_t s_resolving_task;

static void lib__init_locks(void) {
    portENTER_CRITICAL(&s_init_mux);
    if (s_list_lock == NULL) s_list_lock = xSemaphoreCreateMutexStatic(&s_list_lock_storage);
    if (s_load_lock == NULL) s_load_lock = xSemaphoreCreateMutexStatic(&s_load_lock_storage);
    portEXIT_CRITICAL(&s_init_mux);
}

bool lib__resolving(void) {
    return s_resolving_task != NULL && s_resolving_task == xTaskGetCurrentTaskHandle();
}

uintptr_t lib__find_symbol(const char *name) {
    for (const struct esp_elfsym *sym = g_bruce_lib_elfsyms; sym->name != NULL; ++sym) {
        if (strcmp(sym->name, name) == 0) return (uintptr_t)sym->sym;
    }
    ESP_LOGE(TAG, "missing symbol %s", name);
    return 0;
}

static bool lib__valid_name(const char *name) {
    return name != NULL && name[0] != '\0' && strlen(name) < LIB__NAME_MAX && strchr(name, '/') == NULL;
}

/* A build for this chip first, as app_runner does for apps. */
static bool lib__path(const char *name, char *path, size_t capacity) {
    snprintf(path, capacity, "/lib/%s_%s.so", name, CONFIG_IDF_TARGET);
    if (storage__exists_internal(path)) return true;
    snprintf(path, capacity, "/lib/%s.so", name);
    return storage__exists_internal(path);
}

bool lib__exists(const char *name) {
    char path[LIB__PATH_MAX];
    return lib__valid_name(name) && lib__path(name, path, sizeof(path));
}

static bruce_result_t lib__load(lib__entry_t *entry) {
    char path[LIB__PATH_MAX];
    if (!lib__path(entry->name, path, sizeof(path))) return BRUCE_ERR_NOT_FOUND;
    bruce_ext_mem_loader_image_t image;
    bruce_result_t result = ext_mem_loader__stage_path(path, &image);
    if (result != BRUCE_OK) return result;
    int relocated = esp_elf_init(&entry->elf);
    if (relocated == 0) {
        s_resolving_task = xTaskGetCurrentTaskHandle();
#if CONFIG_ELF_LOADER_LOAD_PSRAM
        relocated = esp_elf_relocate(&entry->elf, image.data);
#else
        entry->in_swap = true;
        relocated = esp_elf_relocate_xip(
            &entry->elf, image.data, image.size, &ext_mem_loader__elf_xip_ops, &entry->xip
        );
#endif
        s_resolving_task = NULL;
        if (relocated == 0 && entry->elf.entry == NULL) relocated = -1;
        if (relocated != 0) esp_elf_deinit(&entry->elf);
    }
    (void)ext_mem_loader__release_image(&image);
    if (relocated != 0) {
        ESP_LOGE(TAG, "%s: relocation failed (%d)", path, relocated);
        return BRUCE_ERR_ABI_MISMATCH;
    }
    /* lib_main() takes nothing; esp_elf types every entry point as main(). */
    const void *(*lib_main)(void) = (const void *(*)(void))(uintptr_t)entry->elf.entry;
    entry->api = lib_main();
    if (entry->api == NULL) {
        esp_elf_deinit(&entry->elf);
        return BRUCE_ERR_ABI_MISMATCH;
    }
    ESP_LOGI(TAG, "loaded %s%s", path, entry->in_swap ? " into swap" : "");
    return BRUCE_OK;
}

/* Takes a use of the already loaded library `name`. */
static bruce_result_t lib__acquire(const char *name, lib__entry_t **out) {
    bruce_process_id_t self = process__current_id();
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    lib__entry_t *entry = s_libs;
    while (entry != NULL && strcmp(entry->name, name) != 0) entry = entry->next;
    bruce_result_t result = BRUCE_ERR_NOT_FOUND;
    if (entry != NULL && entry->in_swap && entry->owner != self) {
        result = BRUCE_ERR_BUSY;
    } else if (entry != NULL) {
        entry->users++;
        *out = entry;
        result = BRUCE_OK;
    }
    xSemaphoreGive(s_list_lock);
    return result;
}

/* Drops a use; the last one unloads. `dying`: the owning process is being
 * torn down, and its teardown frees the swap itself. */
static void lib__release(lib__entry_t *entry, bool dying) {
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    bool last = --entry->users == 0;
    if (last) {
        for (lib__entry_t **link = &s_libs; *link != NULL; link = &(*link)->next) {
            if (*link == entry) {
                *link = entry->next;
                break;
            }
        }
    }
    xSemaphoreGive(s_list_lock);
    if (!last) return;
    if (dying && entry->in_swap) entry->elf.xip_handle = 0;
    esp_elf_deinit(&entry->elf);
    ESP_LOGI(TAG, "unloaded %s", entry->name);
    free(entry);
}

static void lib__cleanup(void *context) {
    struct lib__handle *handle = context;
    lib__release(handle->entry, true);
    free(handle);
}

bruce_result_t lib__open(const char *name, lib_handle_t **out_lib, const void **out_api) {
    if (!lib__valid_name(name) || out_lib == NULL || out_api == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_lib = NULL;
    *out_api = NULL;
    lib__init_locks();
    struct lib__handle *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) return BRUCE_ERR_NO_MEMORY;

    xSemaphoreTake(s_load_lock, portMAX_DELAY);
    lib__entry_t *entry = NULL;
    bruce_result_t result = lib__acquire(name, &entry);
    if (result == BRUCE_ERR_NOT_FOUND) {
        entry = calloc(1, sizeof(*entry));
        result = entry != NULL ? BRUCE_OK : BRUCE_ERR_NO_MEMORY;
        if (result == BRUCE_OK) {
            snprintf(entry->name, sizeof(entry->name), "%s", name);
            entry->owner = process__current_id();
            entry->users = 1;
            result = lib__load(entry);
        }
        if (result == BRUCE_OK) {
            xSemaphoreTake(s_list_lock, portMAX_DELAY);
            entry->next = s_libs;
            s_libs = entry;
            xSemaphoreGive(s_list_lock);
        } else {
            free(entry);
            entry = NULL;
        }
    }
    xSemaphoreGive(s_load_lock);

    if (result == BRUCE_OK) {
        handle->entry = entry;
        handle->resource = process_registry__resource_register(lib__cleanup, handle);
        if (handle->resource == BRUCE_RESOURCE_ID_INVALID) {
            lib__release(entry, false);
            result = BRUCE_ERR_NO_MEMORY;
        }
    }
    if (result != BRUCE_OK) {
        free(handle);
        return result;
    }
    *out_lib = handle;
    *out_api = entry->api;
    return BRUCE_OK;
}

void lib__close(lib_handle_t *lib) {
    if (lib == NULL) return;
    (void)process_registry__resource_release(lib->resource);
    lib__release(lib->entry, false);
    free(lib);
}
