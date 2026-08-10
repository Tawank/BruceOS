#include "process_internal.h"

#include "core/storage/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROCESS__ENVIRONMENT_FILE_PATH "/config/.env"
#define PROCESS__ENVIRONMENT_FILE_MAX 4096u

bool process__environment_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') return false;
    size_t length = strlen(name);
    if (length >= BRUCE_ENVIRONMENT_NAME_MAX ||
        !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) {
        return false;
    }
    for (size_t i = 1; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

int process__environment_find(const process__environment_t *environment, const char *name) {
    for (size_t i = 0; i < environment->count; ++i) {
        if (strcmp(environment->entries[i].name, name) == 0) return (int)i;
    }
    return -1;
}

void process__environment_free(process__environment_t *environment) {
    for (size_t i = 0; i < environment->count; ++i) {
        free(environment->entries[i].name);
        free(environment->entries[i].value);
    }
    free(environment->entries);
    memset(environment, 0, sizeof(*environment));
}

bruce_result_t
process__environment_set_locked(process__environment_t *environment, const char *name, const char *value) {
    if (!process__environment_name_valid(name) || value == NULL || strlen(value) >= BRUCE_ENVIRONMENT_VALUE_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    char *value_copy = strdup(value);
    if (value_copy == NULL) return BRUCE_ERR_NO_MEMORY;
    int index = process__environment_find(environment, name);
    if (index >= 0) {
        free(environment->entries[index].value);
        environment->entries[index].value = value_copy;
        return BRUCE_OK;
    }
    if (environment->count >= BRUCE_ENVIRONMENT_MAX_VARIABLES) {
        free(value_copy);
        return BRUCE_ERR_RESOURCE_LIMIT;
    }
    if (environment->count == environment->capacity) {
        size_t capacity = environment->capacity == 0 ? 4 : environment->capacity * 2;
        if (capacity > BRUCE_ENVIRONMENT_MAX_VARIABLES) capacity = BRUCE_ENVIRONMENT_MAX_VARIABLES;
        process__environment_entry_t *grown = realloc(environment->entries, capacity * sizeof(*grown));
        if (grown == NULL) {
            free(value_copy);
            return BRUCE_ERR_NO_MEMORY;
        }
        environment->entries = grown;
        environment->capacity = capacity;
    }
    char *name_copy = strdup(name);
    if (name_copy == NULL) {
        free(value_copy);
        return BRUCE_ERR_NO_MEMORY;
    }
    environment->entries[environment->count++] = (process__environment_entry_t){
        .name = name_copy,
        .value = value_copy,
    };
    return BRUCE_OK;
}

bruce_result_t process__environment_inherit_locked(
    process__record_t *record, const process__record_t *parent, const bruce_environment_variable_t *overlay,
    size_t overlay_count
) {
    if (overlay_count > 0 && overlay == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < s_global_environment.count; ++i) {
        bruce_result_t result = process__environment_set_locked(
            &record->environment, s_global_environment.entries[i].name, s_global_environment.entries[i].value
        );
        if (result != BRUCE_OK) return result;
    }
    if (parent != NULL) {
        for (size_t i = 0; i < parent->environment.count; ++i) {
            bruce_result_t result = process__environment_set_locked(
                &record->environment, parent->environment.entries[i].name, parent->environment.entries[i].value
            );
            if (result != BRUCE_OK) return result;
        }
    }
    for (size_t i = 0; i < overlay_count; ++i) {
        bruce_result_t result = process__environment_set_locked(&record->environment, overlay[i].name, overlay[i].value);
        if (result != BRUCE_OK) return result;
    }
    return BRUCE_OK;
}

static char *process__environment_trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end = '\0';
    return text;
}

bool process__environment_init(void) {
    static const char default_file[] = "# Global process environment defaults.\n"
                                       "# Use NAME=value; blank lines and comments are ignored.\n";
    process__ensure_init();
    char *text = NULL;
    size_t size = 0;
    if (!storage__read_file(PROCESS__ENVIRONMENT_FILE_PATH, &text, &size)) {
        return storage__write_file_atomic(PROCESS__ENVIRONMENT_FILE_PATH, default_file, sizeof(default_file) - 1);
    }
    if (size > PROCESS__ENVIRONMENT_FILE_MAX) {
        printf("Global environment file exceeds %u bytes\n", (unsigned int)PROCESS__ENVIRONMENT_FILE_MAX);
        storage__free(text);
        return false;
    }

    process__environment_t loaded = {0};
    bool success = true;
    unsigned int line_number = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        ++line_number;
        line = process__environment_trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *separator = strchr(line, '=');
        if (separator == NULL) {
            printf("Ignoring invalid %s line %u\n", PROCESS__ENVIRONMENT_FILE_PATH, line_number);
            continue;
        }
        *separator = '\0';
        bruce_result_t result = process__environment_set_locked(
            &loaded, process__environment_trim(line), process__environment_trim(separator + 1)
        );
        if (result == BRUCE_ERR_NO_MEMORY) {
            success = false;
            break;
        }
        if (result != BRUCE_OK) printf("Ignoring invalid %s line %u\n", PROCESS__ENVIRONMENT_FILE_PATH, line_number);
    }
    storage__free(text);
    if (!success) {
        process__environment_free(&loaded);
        return false;
    }
    process__lock();
    process__environment_t previous = s_global_environment;
    s_global_environment = loaded;
    process__unlock();
    process__environment_free(&previous);
    return true;
}

const char *environment__global_get(const char *name) {
    if (!process__environment_name_valid(name)) return NULL;
    process__ensure_init();
    process__lock();
    int index = process__environment_find(&s_global_environment, name);
    const char *value = index >= 0 ? s_global_environment.entries[index].value : NULL;
    process__unlock();
    return value;
}

bruce_result_t environment__global_set(const char *name, const char *value) {
    process__ensure_init();
    process__lock();
    bruce_result_t result = process__environment_set_locked(&s_global_environment, name, value);
    process__unlock();
    return result;
}

static bruce_result_t process__environment_unset_locked(process__environment_t *environment, const char *name) {
    if (!process__environment_name_valid(name)) return BRUCE_ERR_INVALID_ARGUMENT;
    int index = process__environment_find(environment, name);
    if (index >= 0) {
        free(environment->entries[index].name);
        free(environment->entries[index].value);
        size_t last = environment->count - 1;
        if ((size_t)index != last) environment->entries[index] = environment->entries[last];
        environment->count--;
    }
    return BRUCE_OK;
}

bruce_result_t environment__global_unset(const char *name) {
    process__ensure_init();
    process__lock();
    bruce_result_t result = process__environment_unset_locked(&s_global_environment, name);
    process__unlock();
    return result;
}

const char *environment__get(const char *name) {
    if (!process__environment_name_valid(name)) return NULL;
    process__record_t *record = process__current_record();
    if (record == NULL) return NULL;
    process__lock();
    int index = process__environment_find(&record->environment, name);
    const char *value = index >= 0 ? record->environment.entries[index].value : NULL;
    process__unlock();
    return value;
}

bruce_result_t environment__set(const char *name, const char *value) {
    process__record_t *record = process__current_record();
    if (record == NULL) return BRUCE_ERR_INVALID_STATE;
    process__lock();
    bruce_result_t result = process__environment_set_locked(&record->environment, name, value);
    process__unlock();
    return result;
}

bruce_result_t environment__unset(const char *name) {
    process__record_t *record = process__current_record();
    if (record == NULL) return BRUCE_ERR_INVALID_STATE;
    process__lock();
    bruce_result_t result = process__environment_unset_locked(&record->environment, name);
    process__unlock();
    return result;
}

size_t environment__count(void) {
    process__record_t *record = process__current_record();
    return record != NULL ? record->environment.count : 0;
}

bruce_result_t environment__get_at(size_t index, const char **out_name, const char **out_value) {
    if (out_name == NULL || out_value == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    process__record_t *record = process__current_record();
    if (record == NULL) return BRUCE_ERR_INVALID_STATE;
    process__lock();
    if (index >= record->environment.count) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    *out_name = record->environment.entries[index].name;
    *out_value = record->environment.entries[index].value;
    process__unlock();
    return BRUCE_OK;
}
