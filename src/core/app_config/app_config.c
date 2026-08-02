#include "core_sdk/app_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "core/storage/storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#define APP_CONFIG__DIRECTORY "/config"
#define APP_CONFIG__PATH_MAX (8u + BRUCE_APP_CONFIG_NAME_MAX_LEN + 5u + 1u) /* "/config/" NAME ".conf" NUL */
#define APP_CONFIG__SEGMENT_MAX_LEN 31

static StaticSemaphore_t s_app_config_mutex_storage;
static SemaphoreHandle_t s_app_config_mutex;
static portMUX_TYPE s_app_config_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void app_config__ensure_mutex(void) {
    if (s_app_config_mutex != NULL) return;
    portENTER_CRITICAL(&s_app_config_init_mux);
    if (s_app_config_mutex == NULL) {
        s_app_config_mutex = xSemaphoreCreateMutexStatic(&s_app_config_mutex_storage);
    }
    portEXIT_CRITICAL(&s_app_config_init_mux);
}

static void app_config__lock(void) {
    app_config__ensure_mutex();
    xSemaphoreTake(s_app_config_mutex, portMAX_DELAY);
}

static void app_config__unlock(void) { xSemaphoreGive(s_app_config_mutex); }

static bool app_config__name_valid(const char *name) {
    if (name == NULL || name[0] == '\0' || !(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return false;
    }
    size_t length = 1;
    for (const char *p = name + 1; *p != '\0'; ++p, ++length) {
        if (length >= BRUCE_APP_CONFIG_NAME_MAX_LEN) return false;
        if (!(isalnum((unsigned char)*p) || *p == '_')) return false;
    }
    return true;
}

static bool app_config__path_for(const char *app_name, char *out_path, size_t capacity) {
    if (!app_config__name_valid(app_name)) return false;
    int written = snprintf(out_path, capacity, "%s/%s.conf", APP_CONFIG__DIRECTORY, app_name);
    return written > 0 && (size_t)written < capacity;
}

/* Walks a dot-separated json_path, creating intermediate objects when
 * `create` is set. On success, *out_parent is the object owning the final
 * segment and out_key holds that segment name. */
static bool app_config__navigate(
    cJSON *root, const char *json_path, bool create, cJSON **out_parent, char *out_key, size_t key_capacity
) {
    if (root == NULL || json_path == NULL || json_path[0] == '\0') return false;
    cJSON *current = root;
    const char *segment = json_path;
    for (;;) {
        const char *dot = strchr(segment, '.');
        size_t length = dot != NULL ? (size_t)(dot - segment) : strlen(segment);
        if (length == 0 || length > APP_CONFIG__SEGMENT_MAX_LEN || length >= key_capacity) return false;
        if (!(isalpha((unsigned char)segment[0]) || segment[0] == '_')) return false;
        for (size_t i = 1; i < length; ++i) {
            if (!(isalnum((unsigned char)segment[i]) || segment[i] == '_')) return false;
        }

        char name[APP_CONFIG__SEGMENT_MAX_LEN + 1];
        memcpy(name, segment, length);
        name[length] = '\0';

        if (dot == NULL) {
            memcpy(out_key, name, length + 1);
            *out_parent = current;
            return true;
        }

        cJSON *child = cJSON_GetObjectItemCaseSensitive(current, name);
        if (child == NULL) {
            if (!create) return false;
            child = cJSON_AddObjectToObject(current, name);
            if (child == NULL) return false;
        } else if (!cJSON_IsObject(child)) {
            return false;
        }
        current = child;
        segment = dot + 1;
    }
}

static cJSON *app_config__load(const char *app_name) {
    char path[APP_CONFIG__PATH_MAX];
    if (!app_config__path_for(app_name, path, sizeof(path))) return NULL;

    char *text = NULL;
    size_t size = 0;
    cJSON *root = NULL;
    if (storage__read_file(path, &text, &size) && size > 0) {
        root = cJSON_ParseWithLength(text, size);
    }
    if (text != NULL) storage__free(text);

    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

static bool app_config__save(const char *app_name, cJSON *root) {
    char path[APP_CONFIG__PATH_MAX];
    if (!app_config__path_for(app_name, path, sizeof(path))) return false;
    if (!storage__mkdir_internal(APP_CONFIG__DIRECTORY)) return false;

    char *text = cJSON_Print(root);
    if (text == NULL) return false;
    bool saved = storage__write_file_atomic(path, text, strlen(text));
    cJSON_free(text);
    return saved;
}

/* ------------------------------------------------------------------------ */
/* Scalars                                                                   */
/* ------------------------------------------------------------------------ */

bool app_config__get_bool(const char *app_name, const char *json_path, bool default_value) {
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    bool value = default_value;
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    if (app_config__navigate(root, json_path, false, &parent, key, sizeof(key))) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (cJSON_IsBool(item)) value = cJSON_IsTrue(item);
    }
    cJSON_Delete(root);
    app_config__unlock();
    return value;
}

bruce_result_t app_config__set_bool(const char *app_name, const char *json_path, bool value) {
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    if (root != NULL && app_config__navigate(root, json_path, true, &parent, key, sizeof(key))) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        result = cJSON_AddBoolToObject(parent, key, value) != NULL
                     ? (app_config__save(app_name, root) ? BRUCE_OK : BRUCE_ERR_IO)
                     : BRUCE_ERR_NO_MEMORY;
    }
    cJSON_Delete(root);
    app_config__unlock();
    return result;
}

int app_config__get_int(const char *app_name, const char *json_path, int default_value) {
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    int value = default_value;
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    if (app_config__navigate(root, json_path, false, &parent, key, sizeof(key))) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (cJSON_IsNumber(item)) value = item->valueint;
    }
    cJSON_Delete(root);
    app_config__unlock();
    return value;
}

bruce_result_t app_config__set_int(const char *app_name, const char *json_path, int value) {
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    if (root != NULL && app_config__navigate(root, json_path, true, &parent, key, sizeof(key))) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        result = cJSON_AddNumberToObject(parent, key, value) != NULL
                     ? (app_config__save(app_name, root) ? BRUCE_OK : BRUCE_ERR_IO)
                     : BRUCE_ERR_NO_MEMORY;
    }
    cJSON_Delete(root);
    app_config__unlock();
    return result;
}

bool app_config__get_string(
    const char *app_name, const char *json_path, const char *default_value, char *out_value,
    size_t capacity
) {
    if (out_value == NULL || capacity == 0) return false;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    bool found = false;
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    if (app_config__navigate(root, json_path, false, &parent, key, sizeof(key))) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            size_t length = strlen(item->valuestring);
            if (length >= capacity) length = capacity - 1;
            memcpy(out_value, item->valuestring, length);
            out_value[length] = '\0';
            found = true;
        }
    }
    cJSON_Delete(root);
    app_config__unlock();

    if (!found) {
        const char *fallback = default_value != NULL ? default_value : "";
        size_t length = strlen(fallback);
        if (length >= capacity) length = capacity - 1;
        memcpy(out_value, fallback, length);
        out_value[length] = '\0';
    }
    return found;
}

bruce_result_t app_config__set_string(const char *app_name, const char *json_path, const char *value) {
    if (value == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    if (root != NULL && app_config__navigate(root, json_path, true, &parent, key, sizeof(key))) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
        result = cJSON_AddStringToObject(parent, key, value) != NULL
                     ? (app_config__save(app_name, root) ? BRUCE_OK : BRUCE_ERR_IO)
                     : BRUCE_ERR_NO_MEMORY;
    }
    cJSON_Delete(root);
    app_config__unlock();
    return result;
}

/* ------------------------------------------------------------------------ */
/* Arrays                                                                    */
/* ------------------------------------------------------------------------ */

size_t app_config__get_bool_array(
    const char *app_name, const char *json_path, bool *out_values, size_t capacity
) {
    if (out_values == NULL || capacity == 0) return 0;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    size_t count = 0;
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    if (app_config__navigate(root, json_path, false, &parent, key, sizeof(key))) {
        cJSON *array = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (cJSON_IsArray(array)) {
            cJSON *item;
            cJSON_ArrayForEach(item, array) {
                if (count >= capacity) break;
                if (!cJSON_IsBool(item)) continue;
                out_values[count++] = cJSON_IsTrue(item);
            }
        }
    }
    cJSON_Delete(root);
    app_config__unlock();
    return count;
}

bruce_result_t
app_config__set_bool_array(const char *app_name, const char *json_path, const bool *values, size_t count) {
    if (values == NULL && count > 0) return BRUCE_ERR_INVALID_ARGUMENT;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    if (root != NULL && app_config__navigate(root, json_path, true, &parent, key, sizeof(key))) {
        cJSON *array = cJSON_CreateArray();
        bool ok = array != NULL;
        for (size_t i = 0; ok && i < count; ++i) {
            ok = cJSON_AddItemToArray(array, cJSON_CreateBool(values[i]));
        }
        if (ok) {
            cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
            ok = cJSON_AddItemToObject(parent, key, array);
        }
        if (ok) result = app_config__save(app_name, root) ? BRUCE_OK : BRUCE_ERR_IO;
        else {
            cJSON_Delete(array);
            result = BRUCE_ERR_NO_MEMORY;
        }
    }
    cJSON_Delete(root);
    app_config__unlock();
    return result;
}

size_t
app_config__get_int_array(const char *app_name, const char *json_path, int *out_values, size_t capacity) {
    if (out_values == NULL || capacity == 0) return 0;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    size_t count = 0;
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    if (app_config__navigate(root, json_path, false, &parent, key, sizeof(key))) {
        cJSON *array = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (cJSON_IsArray(array)) {
            cJSON *item;
            cJSON_ArrayForEach(item, array) {
                if (count >= capacity) break;
                if (!cJSON_IsNumber(item)) continue;
                out_values[count++] = item->valueint;
            }
        }
    }
    cJSON_Delete(root);
    app_config__unlock();
    return count;
}

bruce_result_t
app_config__set_int_array(const char *app_name, const char *json_path, const int *values, size_t count) {
    if (values == NULL && count > 0) return BRUCE_ERR_INVALID_ARGUMENT;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    if (root != NULL && app_config__navigate(root, json_path, true, &parent, key, sizeof(key))) {
        cJSON *array = cJSON_CreateArray();
        bool ok = array != NULL;
        for (size_t i = 0; ok && i < count; ++i) {
            ok = cJSON_AddItemToArray(array, cJSON_CreateNumber(values[i]));
        }
        if (ok) {
            cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
            ok = cJSON_AddItemToObject(parent, key, array);
        }
        if (ok) result = app_config__save(app_name, root) ? BRUCE_OK : BRUCE_ERR_IO;
        else {
            cJSON_Delete(array);
            result = BRUCE_ERR_NO_MEMORY;
        }
    }
    cJSON_Delete(root);
    app_config__unlock();
    return result;
}

size_t app_config__get_string_array(
    const char *app_name, const char *json_path, char *const *out_values, size_t value_size,
    size_t capacity
) {
    if (out_values == NULL || value_size == 0 || capacity == 0) return 0;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    size_t count = 0;
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    if (app_config__navigate(root, json_path, false, &parent, key, sizeof(key))) {
        cJSON *array = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (cJSON_IsArray(array)) {
            cJSON *item;
            cJSON_ArrayForEach(item, array) {
                if (count >= capacity) break;
                if (!cJSON_IsString(item) || item->valuestring == NULL) continue;
                size_t length = strlen(item->valuestring);
                if (length >= value_size) length = value_size - 1;
                memcpy(out_values[count], item->valuestring, length);
                out_values[count][length] = '\0';
                ++count;
            }
        }
    }
    cJSON_Delete(root);
    app_config__unlock();
    return count;
}

bruce_result_t app_config__set_string_array(
    const char *app_name, const char *json_path, const char *const *values, size_t count
) {
    if (values == NULL && count > 0) return BRUCE_ERR_INVALID_ARGUMENT;
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    if (root != NULL && app_config__navigate(root, json_path, true, &parent, key, sizeof(key))) {
        cJSON *array = cJSON_CreateArray();
        bool ok = array != NULL;
        for (size_t i = 0; ok && i < count; ++i) {
            cJSON *item = cJSON_CreateString(values[i] != NULL ? values[i] : "");
            ok = item != NULL && cJSON_AddItemToArray(array, item);
        }
        if (ok) {
            cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
            ok = cJSON_AddItemToObject(parent, key, array);
        }
        if (ok) result = app_config__save(app_name, root) ? BRUCE_OK : BRUCE_ERR_IO;
        else {
            cJSON_Delete(array);
            result = BRUCE_ERR_NO_MEMORY;
        }
    }
    cJSON_Delete(root);
    app_config__unlock();
    return result;
}

bruce_result_t app_config__remove(const char *app_name, const char *json_path) {
    app_config__lock();
    cJSON *root = app_config__load(app_name);
    cJSON *parent = NULL;
    char key[APP_CONFIG__SEGMENT_MAX_LEN + 1];
    bruce_result_t result = BRUCE_ERR_INVALID_ARGUMENT;
    if (app_config__navigate(root, json_path, false, &parent, key, sizeof(key))) {
        if (cJSON_GetObjectItemCaseSensitive(parent, key) != NULL) {
            cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
            result = app_config__save(app_name, root) ? BRUCE_OK : BRUCE_ERR_IO;
        } else {
            result = BRUCE_ERR_NOT_FOUND;
        }
    }
    cJSON_Delete(root);
    app_config__unlock();
    return result;
}
