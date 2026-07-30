#include "permission.h"

#include "core_sdk/permission.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "core/storage/storage.h"
#include "core/process/process.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"

#define PERMISSION__FILE_PATH "/permissions.json"
#define PERMISSION__MAX_FILES 32

typedef struct {
    char file_name[BRUCE_PERMISSION_FILE_NAME_MAX];
    bool known[BRUCE_PERMISSION_COUNT];
    bool allowed[BRUCE_PERMISSION_COUNT];
} permission__file_entry_t;

static const char *const s_permission_names[BRUCE_PERMISSION_COUNT] = {
    [BRUCE_PERMISSION_HTTP] = "http",
    [BRUCE_PERMISSION_WIFI] = "wifi",
    [BRUCE_PERMISSION_BT] = "bt",
    [BRUCE_PERMISSION_GPS] = "gps",
    [BRUCE_PERMISSION_RF] = "rf",
    [BRUCE_PERMISSION_INPUT] = "input",
    [BRUCE_PERMISSION_GPIO] = "gpio",
    [BRUCE_PERMISSION_IR] = "ir",
    [BRUCE_PERMISSION_RFID] = "rfid",
    [BRUCE_PERMISSION_MICROPHONE] = "microphone",
    [BRUCE_PERMISSION_HID] = "hid",
    [BRUCE_PERMISSION_EXECUTE] = "execute",
    [BRUCE_PERMISSION_PROCESS] = "process",
    [BRUCE_PERMISSION_STORAGE] = "storage",
    [BRUCE_PERMISSION_CONFIG] = "config",
    [BRUCE_PERMISSION_SERIAL] = "serial",
    [BRUCE_PERMISSION_SSH] = "ssh",
};

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static permission__file_entry_t *s_files;
static size_t s_file_count;
static size_t s_file_capacity;
static bool s_loaded;
/* Task currently inside permission__prompt(); nested checks from it fail
 * closed so the dialog cannot recurse into another prompt. */
static TaskHandle_t s_prompt_task;

static void permission__lock(void) {
    if (s_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void permission__unlock(void) { xSemaphoreGive(s_mutex); }

const char *permission__name(bruce_permission_t permission) {
    if (permission < 0 || permission >= BRUCE_PERMISSION_COUNT) return NULL;
    return s_permission_names[permission];
}

bool permission__from_name(const char *name, bruce_permission_t *out_permission) {
    if (name == NULL || out_permission == NULL) return false;
    for (int i = 0; i < BRUCE_PERMISSION_COUNT; ++i) {
        if (strcmp(s_permission_names[i], name) == 0) {
            *out_permission = (bruce_permission_t)i;
            return true;
        }
    }
    return false;
}

/* Caller must hold the lock. */
static permission__file_entry_t *permission__find_locked(const char *file_name) {
    for (size_t i = 0; i < s_file_count; ++i) {
        if (strcmp(s_files[i].file_name, file_name) == 0) return &s_files[i];
    }
    return NULL;
}

/* Caller must hold the lock. Returns NULL if the table is full and
 * `file_name` has no existing entry. */
static permission__file_entry_t *permission__find_or_create_locked(const char *file_name) {
    permission__file_entry_t *entry = permission__find_locked(file_name);
    if (entry != NULL) return entry;
    if (s_file_count == PERMISSION__MAX_FILES) return NULL;
    if (s_file_count == s_file_capacity) {
        size_t next_capacity = s_file_capacity == 0 ? 4u : s_file_capacity * 2u;
        if (next_capacity > PERMISSION__MAX_FILES) next_capacity = PERMISSION__MAX_FILES;
        permission__file_entry_t *grown = realloc(s_files, next_capacity * sizeof(*s_files));
        if (grown == NULL) return NULL;
        s_files = grown;
        s_file_capacity = next_capacity;
    }
    entry = &s_files[s_file_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->file_name, file_name, sizeof(entry->file_name) - 1);
    return entry;
}

/* Caller must hold the lock. */
static bruce_result_t permission__load_locked(void) {
    if (s_loaded) return BRUCE_OK;
    s_loaded = true;

    char *text = NULL;
    size_t size = 0;
    if (!storage__read_file(PERMISSION__FILE_PATH, &text, &size) || size == 0) {
        if (text != NULL) storage__free(text);
        return BRUCE_OK;
    }

    cJSON *root = cJSON_ParseWithLength(text, size);
    storage__free(text);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root != NULL) cJSON_Delete(root);
        return BRUCE_OK;
    }

    const cJSON *file_item;
    cJSON_ArrayForEach(file_item, root) {
        if (file_item->string == NULL || !cJSON_IsObject(file_item)) continue;
        permission__file_entry_t *entry = permission__find_or_create_locked(file_item->string);
        if (entry == NULL) {
            if (s_file_count == PERMISSION__MAX_FILES) continue;
            free(s_files);
            s_files = NULL;
            s_file_count = 0;
            s_file_capacity = 0;
            cJSON_Delete(root);
            s_loaded = false;
            return BRUCE_ERR_NO_MEMORY;
        }
        const cJSON *perm_item;
        cJSON_ArrayForEach(perm_item, file_item) {
            if (perm_item->string == NULL || !cJSON_IsNumber(perm_item)) continue;
            bruce_permission_t permission;
            if (!permission__from_name(perm_item->string, &permission)) continue;
            entry->known[permission] = true;
            entry->allowed[permission] = perm_item->valueint != 0;
        }
    }
    cJSON_Delete(root);
    return BRUCE_OK;
}

static bruce_result_t permission__ensure_loaded(void) {
    permission__lock();
    bruce_result_t result = permission__load_locked();
    permission__unlock();
    return result;
}

/* Caller must hold the lock. */
static bool permission__save_locked(void) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return false;

    for (size_t i = 0; i < s_file_count; ++i) {
        cJSON *file_obj = cJSON_AddObjectToObject(root, s_files[i].file_name);
        if (file_obj == NULL) continue;
        for (int p = 0; p < BRUCE_PERMISSION_COUNT; ++p) {
            if (!s_files[i].known[p]) continue;
            cJSON_AddNumberToObject(file_obj, s_permission_names[p], s_files[i].allowed[p] ? 1 : 0);
        }
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL) return false;
    bool saved = storage__write_file_atomic(PERMISSION__FILE_PATH, text, strlen(text));
    cJSON_free(text);
    return saved;
}

static bool permission__save(void) {
    permission__lock();
    bool saved = permission__save_locked();
    permission__unlock();
    return saved;
}

/* Shows a single allow/deny dialog__choice() for `file_name`/`permission`.
 * Returns true only if the dialog succeeded and the user picked "Allow"
 * (choice index 0); *out_answered reports whether the dialog produced any
 * usable answer at all, so the caller can decide whether to persist it. */
static bool permission__prompt(const char *file_name, bruce_permission_t permission, bool *out_answered) {
    bruce_dialog_choice_t choices[2] = {
        {.label = "Allow", .value = "allow"},
        {.label = "Deny",  .value = "deny" },
    };
    char message[160];
    snprintf(message, sizeof(message), "%s requests %s permission", file_name, permission__name(permission));

    size_t selected = 1;
    bruce_result_t result = dialog__choice("Permission request", message, choices, 2, &selected, NULL);
    *out_answered = result == BRUCE_OK;
    return result == BRUCE_OK && selected == 0;
}

/* Runs permission__prompt() with a reentrancy guard: the dialog re-enters
 * permission__check() through Core UI paths (dialog theme colors ->
 * config__get_pri_color -> config__guard), and re-prompting there would
 * recurse until the task stack overflows. A nested attempt reports
 * "unanswered" so callers fail closed without showing a second dialog. */
static bool permission__prompt_guarded(const char *file_name, bruce_permission_t permission, bool *out_answered) {
    if (s_prompt_task == xTaskGetCurrentTaskHandle()) {
        *out_answered = false;
        return false;
    }
    s_prompt_task = xTaskGetCurrentTaskHandle();
    bool allowed = permission__prompt(file_name, permission, out_answered);
    s_prompt_task = NULL;
    return allowed;
}

bruce_result_t permission__check(bruce_permission_t permission) {
    if (permission < 0 || permission >= BRUCE_PERMISSION_COUNT) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bool built_in = false;
    char key[BRUCE_PERMISSION_FILE_NAME_MAX] = {0};
    if (process_registry__current_context(&built_in, key, sizeof(key), NULL) != BRUCE_OK) {
        /* No Core process context at all (e.g. called during boot): treat the
         * same as a built-in's implicit grant. */
        return BRUCE_OK;
    }
    if (built_in) { return BRUCE_OK; }
    if (key[0] == '\0') { return BRUCE_ERR_PERMISSION; }

    bruce_result_t loaded = permission__ensure_loaded();
    if (loaded != BRUCE_OK) return loaded;

    permission__lock();
    permission__file_entry_t *entry = permission__find_locked(key);
    if (entry != NULL && entry->known[permission]) {
        bool allowed = entry->allowed[permission];
        permission__unlock();
        return allowed ? BRUCE_OK : BRUCE_ERR_PERMISSION;
    }
    permission__unlock(); /* dialog__choice() must not run while holding the lock */

    bool answered = false;
    bool allowed = permission__prompt_guarded(key, permission, &answered);
    if (!answered) {
        /* Indeterminate: don't persist, let a later call try again. */
        return BRUCE_ERR_PERMISSION;
    }

    permission__lock();
    entry = permission__find_or_create_locked(key);
    if (entry == NULL) {
        bruce_result_t result =
            s_file_count == PERMISSION__MAX_FILES ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_ERR_NO_MEMORY;
        permission__unlock();
        return result;
    }
    entry->known[permission] = true;
    entry->allowed[permission] = allowed;
    permission__unlock();
    permission__save();

    return allowed ? BRUCE_OK : BRUCE_ERR_PERMISSION;
}

bruce_result_t
permission__preflight(const char *file_name, const char *const *permission_names, size_t count) {
    if (file_name == NULL || file_name[0] == '\0' || strlen(file_name) >= BRUCE_PERMISSION_FILE_NAME_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (count > 0 && permission_names == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    if (count > BRUCE_PERMISSION_COUNT) { return BRUCE_ERR_INVALID_ARGUMENT; }

    bruce_permission_t resolved[BRUCE_PERMISSION_COUNT];
    for (size_t i = 0; i < count; ++i) {
        if (!permission__from_name(permission_names[i], &resolved[i])) { return BRUCE_ERR_INVALID_ARGUMENT; }
    }

    bruce_result_t loaded = permission__ensure_loaded();
    if (loaded != BRUCE_OK) return loaded;

    for (size_t i = 0; i < count; ++i) {
        permission__lock();
        permission__file_entry_t *entry = permission__find_locked(file_name);
        bool already_known = entry != NULL && entry->known[resolved[i]];
        permission__unlock();
        if (already_known) continue;

        bool answered = false;
        bool allowed = permission__prompt_guarded(file_name, resolved[i], &answered);
        if (!answered) continue; /* leave unresolved; a dynamic request may retry it later */

        permission__lock();
        entry = permission__find_or_create_locked(file_name);
        if (entry == NULL) {
            bruce_result_t result =
                s_file_count == PERMISSION__MAX_FILES ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_ERR_NO_MEMORY;
            permission__unlock();
            return result;
        }
        entry->known[resolved[i]] = true;
        entry->allowed[resolved[i]] = allowed;
        permission__unlock();
    }

    permission__save();
    return BRUCE_OK;
}

bruce_result_t
permission__get_saved(const char *file_name, bruce_permission_t permission, bool *out_allowed) {
    if (file_name == NULL || file_name[0] == '\0' || permission < 0 || permission >= BRUCE_PERMISSION_COUNT ||
        out_allowed == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_result_t loaded = permission__ensure_loaded();
    if (loaded != BRUCE_OK) return loaded;

    permission__lock();
    permission__file_entry_t *entry = permission__find_locked(file_name);
    bruce_result_t result;
    if (entry == NULL || !entry->known[permission]) {
        result = BRUCE_ERR_NOT_FOUND;
    } else {
        *out_allowed = entry->allowed[permission];
        result = BRUCE_OK;
    }
    permission__unlock();
    return result;
}

bruce_result_t permission__set(const char *file_name, bruce_permission_t permission, bool allowed) {
    if (file_name == NULL || file_name[0] == '\0' || strlen(file_name) >= BRUCE_PERMISSION_FILE_NAME_MAX ||
        permission < 0 || permission >= BRUCE_PERMISSION_COUNT) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_result_t loaded = permission__ensure_loaded();
    if (loaded != BRUCE_OK) return loaded;

    permission__lock();
    permission__file_entry_t *entry = permission__find_or_create_locked(file_name);
    if (entry == NULL) {
        bruce_result_t result =
            s_file_count == PERMISSION__MAX_FILES ? BRUCE_ERR_RESOURCE_LIMIT : BRUCE_ERR_NO_MEMORY;
        permission__unlock();
        return result;
    }
    entry->known[permission] = true;
    entry->allowed[permission] = allowed;
    permission__unlock();

    return permission__save() ? BRUCE_OK : BRUCE_ERR_IO;
}

bool permission__test_reset(void) {
    permission__lock();
    free(s_files);
    s_files = NULL;
    s_file_count = 0;
    s_file_capacity = 0;
    s_loaded = true; /* prevent a later ensure_loaded from reloading the old file before removal completes */
    permission__unlock();

    if (storage__remove_internal(PERMISSION__FILE_PATH)) return true;
    return !storage__exists(PERMISSION__FILE_PATH);
}
