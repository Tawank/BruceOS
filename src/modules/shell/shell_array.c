#include "shell_array.h"

#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/stdio.h"
#include "shell_parser.h"

int shell_array__find(const shell_state_t *state, const char *name) {
    for (size_t i = 0; i < state->array_count; ++i) {
        if (strcmp(state->arrays[i].name, name) == 0) return (int)i;
    }
    return -1;
}

const char *shell_array__get(const shell_state_t *state, const char *name, size_t index) {
    int found = shell_array__find(state, name);
    if (found < 0) return NULL;
    const shell_array_t *array = &state->arrays[found];
    return index < array->count ? array->elements[index] : NULL;
}

size_t shell_array__length(const shell_state_t *state, const char *name) {
    int found = shell_array__find(state, name);
    if (found < 0) return 0;
    const shell_array_t *array = &state->arrays[found];
    size_t length = 0;
    for (size_t i = 0; i < array->count; ++i) {
        if (array->elements[i] != NULL) length++;
    }
    return length;
}

size_t shell_array__index_bound(const shell_state_t *state, const char *name) {
    int found = shell_array__find(state, name);
    return found < 0 ? 0 : state->arrays[found].count;
}

static char *shell_array__dup(const char *text) {
    size_t length = strlen(text);
    char *copy = memory__malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

/* Finds `name` among state->arrays, creating a fresh (empty) one if it
 * doesn't exist yet -- shared by every mutating entry point below. Returns
 * NULL (with an error already printed) on an invalid name, a full array
 * table, or out of memory; the caller's own return convention (0/1/2) maps
 * onto that the same way shell_builtins__set() already does for scalars. */
static shell_array_t *shell_array__find_or_create(shell_state_t *state, const char *name) {
    int found = shell_array__find(state, name);
    if (found >= 0) return &state->arrays[found];
    size_t name_length = strlen(name);
    if (!shell_parser__valid_name(name, name_length) || name_length >= SHELL__ARRAY_NAME_MAX) {
        stdio__printf("shell: %s: invalid array name\n", name);
        return NULL;
    }
    if (state->array_count >= SHELL__MAX_ARRAYS) {
        stdio__printf("shell: too many arrays\n");
        return NULL;
    }
    if (state->array_count >= state->array_capacity) {
        size_t new_capacity = state->array_capacity == 0 ? 4 : state->array_capacity * 2;
        if (new_capacity > SHELL__MAX_ARRAYS) new_capacity = SHELL__MAX_ARRAYS;
        shell_array_t *grown = memory__realloc(state->arrays, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            stdio__printf("shell: out of memory\n");
            return NULL;
        }
        state->arrays = grown;
        state->array_capacity = new_capacity;
    }
    char *name_copy = shell_array__dup(name);
    if (name_copy == NULL) {
        stdio__printf("shell: out of memory\n");
        return NULL;
    }
    shell_array_t *array = &state->arrays[state->array_count++];
    array->name = name_copy;
    array->elements = NULL;
    array->count = 0;
    array->capacity = 0;
    return array;
}

/* Grows `array`'s backing storage, if needed, so elements[0..index] are all
 * valid slots -- newly-exposed slots past the old count are zeroed (NULL,
 * i.e. holes), matching bash extending an array past its current length.
 * Returns false (with an error already printed) on an oversized index or
 * out of memory. */
static bool shell_array__grow(shell_array_t *array, size_t index) {
    if (index >= SHELL__MAX_ARRAY_ELEMENTS) {
        stdio__printf("shell: %s: array index too large\n", array->name);
        return false;
    }
    if (index >= array->capacity) {
        size_t new_capacity = array->capacity == 0 ? 4 : array->capacity * 2;
        while (new_capacity <= index) new_capacity *= 2;
        if (new_capacity > SHELL__MAX_ARRAY_ELEMENTS) new_capacity = SHELL__MAX_ARRAY_ELEMENTS;
        char **grown = memory__realloc(array->elements, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            stdio__printf("shell: out of memory\n");
            return false;
        }
        for (size_t i = array->capacity; i < new_capacity; ++i) grown[i] = NULL;
        array->elements = grown;
        array->capacity = new_capacity;
    }
    if (index >= array->count) array->count = index + 1;
    return true;
}

int shell_array__set(shell_state_t *state, const char *name, size_t index, const char *value) {
    if (strlen(value) >= SHELL__ARRAY_ELEMENT_MAX) {
        stdio__printf("shell: %s: array element too long\n", name);
        return 2;
    }
    shell_array_t *array = shell_array__find_or_create(state, name);
    if (array == NULL) return 1;
    char *value_copy = shell_array__dup(value);
    if (value_copy == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    if (!shell_array__grow(array, index)) {
        memory__free(value_copy);
        return 1;
    }
    memory__free(array->elements[index]);
    array->elements[index] = value_copy;
    return 0;
}

int shell_array__append(shell_state_t *state, const char *name, const char *value) {
    int found = shell_array__find(state, name);
    size_t next_index = found >= 0 ? state->arrays[found].count : 0;
    return shell_array__set(state, name, next_index, value);
}

int shell_array__assign(shell_state_t *state, const char *name, char *const *values, size_t count) {
    shell_array_t *array = shell_array__find_or_create(state, name);
    if (array == NULL) return 1;
    /* Freed slots are set back to NULL, not just left dangling -- the loop
     * below calls shell_array__set() for each new element, which itself
     * frees whatever already occupies that slot before overwriting it (see
     * its own doc comment); without this, that would free the very same
     * pointer this loop just freed, a double free on any index this call
     * reuses (every index below `count`, i.e. the common case of
     * reassigning an array that already had elements). */
    for (size_t i = 0; i < array->count; ++i) {
        memory__free(array->elements[i]);
        array->elements[i] = NULL;
    }
    array->count = 0;
    for (size_t i = 0; i < count; ++i) {
        int status = shell_array__set(state, name, i, values[i]);
        if (status != 0) return status;
    }
    return 0;
}

void shell_array__unset_index(shell_state_t *state, const char *name, size_t index) {
    int found = shell_array__find(state, name);
    if (found < 0) return;
    shell_array_t *array = &state->arrays[found];
    if (index >= array->count) return;
    memory__free(array->elements[index]);
    array->elements[index] = NULL;
}

static void shell_array__remove_at(shell_state_t *state, size_t index) {
    shell_array_t *array = &state->arrays[index];
    for (size_t i = 0; i < array->count; ++i) memory__free(array->elements[i]);
    memory__free(array->elements);
    memory__free(array->name);
    size_t last = state->array_count - 1;
    if (index != last) state->arrays[index] = state->arrays[last];
    state->array_count--;
}

void shell_array__unset(shell_state_t *state, const char *name) {
    int found = shell_array__find(state, name);
    if (found >= 0) shell_array__remove_at(state, (size_t)found);
}

char *shell_array__join(const shell_state_t *state, const char *name) {
    int found = shell_array__find(state, name);
    if (found < 0) return NULL;
    const shell_array_t *array = &state->arrays[found];
    size_t needed = 1; /* the trailing NUL */
    for (size_t i = 0; i < array->count; ++i) {
        if (array->elements[i] == NULL) continue;
        needed += strlen(array->elements[i]) + 1; /* +1 for a separating space */
    }
    char *joined = memory__malloc(needed);
    if (joined == NULL) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < array->count; ++i) {
        if (array->elements[i] == NULL) continue;
        if (used > 0) joined[used++] = ' ';
        size_t element_length = strlen(array->elements[i]);
        memcpy(joined + used, array->elements[i], element_length);
        used += element_length;
    }
    joined[used] = '\0';
    return joined;
}

void shell_array__free_all(shell_state_t *state) {
    for (size_t i = 0; i < state->array_count; ++i) {
        shell_array_t *array = &state->arrays[i];
        for (size_t e = 0; e < array->count; ++e) memory__free(array->elements[e]);
        memory__free(array->elements);
        memory__free(array->name);
    }
    memory__free(state->arrays);
    state->arrays = NULL;
    state->array_count = 0;
    state->array_capacity = 0;
}
